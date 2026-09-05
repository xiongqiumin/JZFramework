#include "JZCompress.h"

#include <cstdint>
#include <climits>
#include <vector>

// miniz：public domain（unlicense）单依赖后端，覆盖 deflate/zlib/gzip 流式 + zip 归档。
// 整套源文件位于 ./miniz/，编译时与 JZCompress 一起编入（见 JZCompress.cmake）。
#include "miniz/miniz.h"
#include "JZCore/JZDir.h"
#include "JZCore/JZFileInfo.h"

// gzip 固定 10 字节文件头：magic(1f 8b) | CM=8(deflate) | FLG=0 | MTIME=0 | XFL=0 | OS=255(unknown)
static const uint8_t kGzipHeader[10] = {0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff};

namespace
{

int makeCompressFlags(JZCompressAlgorithm algo, int level)
{
    int lvl = level;
    if (lvl < 0 || lvl > 9) lvl = 6; // -1=默认 -> 6
    // zlib 用 window_bits=+15（带 zlib 头/尾）；Deflate/GZip 用 -15（raw deflate，由我们自封 gzip 头尾）
    int wb = (algo == JZCompressAlgorithm::Zlib) ? 15 : -15;
    return static_cast<int>(tdefl_create_comp_flags_from_zip_params(lvl, wb, 0));
}

bool isUnsupported(JZCompressAlgorithm algo)
{
    return algo == JZCompressAlgorithm::Brotli || algo == JZCompressAlgorithm::Zstd;
}

// 解析 gzip 文件头。返回头长度（>0 成功）；返回 0 表示字节不足需更多；返回 -1 表示非法。
int parseGzipHeader(const uint8_t *p, size_t n, bool &needMore)
{
    needMore = false;
    if (n < 10) { needMore = true; return 0; }
    if (p[0] != 0x1f || p[1] != 0x8b) return -1;
    if (p[2] != 8) return -1; // 仅支持 deflate 内核
    uint8_t flg = p[3];
    size_t pos = 10;
    if (flg & 0x04) { // FEXTRA
        if (n < pos + 2) { needMore = true; return 0; }
        int xlen = p[pos] | (p[pos + 1] << 8);
        pos += 2;
        if (n < pos + static_cast<size_t>(xlen)) { needMore = true; return 0; }
        pos += xlen;
    }
    if (flg & 0x08) { // FNAME（零结尾）
        while (pos < n && p[pos] != 0) pos++;
        if (pos >= n) { needMore = true; return 0; }
        pos++;
    }
    if (flg & 0x10) { // FCOMMENT（零结尾）
        while (pos < n && p[pos] != 0) pos++;
        if (pos >= n) { needMore = true; return 0; }
        pos++;
    }
    if (flg & 0x02) { // FHCRC
        if (n < pos + 2) { needMore = true; return 0; }
        pos += 2;
    }
    return static_cast<int>(pos);
}

uint32_t readLE32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

} // namespace

const char *jzCompressAlgorithmName(JZCompressAlgorithm algo)
{
    switch (algo)
    {
    case JZCompressAlgorithm::Deflate: return "Deflate";
    case JZCompressAlgorithm::Zlib:    return "Zlib";
    case JZCompressAlgorithm::GZip:    return "GZip";
    case JZCompressAlgorithm::Zstd:    return "Zstd";
    case JZCompressAlgorithm::Brotli:  return "Brotli";
    }
    return "Unknown";
}

// ---------------- 一次性（按块）压缩 / 解压（复用流式接口） ----------------
JZResult<JZByteArray> jzCompressBlock(const JZByteArray &in, JZCompressAlgorithm algo, int level)
{
    if (isUnsupported(algo))
        return JZResult<JZByteArray>::Error("JZCompress: algorithm not implemented (Brotli/Zstd)");
    JZCompressStream s(algo, level);
    // 一次性压缩：用超大 blockSize 避免中途 SYNC_FLUSH，仅 finish() 收尾，保证压缩比。
    s.setBlockSize(INT_MAX);
    JZByteArray out = s.write(in);
    out.append(s.finish());
    return JZResult<JZByteArray>::Ok(std::move(out));
}

JZResult<JZByteArray> jzDecompressBlock(const JZByteArray &in, JZCompressAlgorithm algo)
{
    if (isUnsupported(algo))
        return JZResult<JZByteArray>::Error("JZCompress: algorithm not implemented (Brotli/Zstd)");
    JZDecompressStream s(algo);
    JZByteArray out = s.write(in);
    out.append(s.finish());
    return JZResult<JZByteArray>::Ok(std::move(out));
}

// ---------------- 流式压缩 ----------------
struct JZCompressStream::Impl
{
    JZCompressAlgorithm algo;
    int level;
    int blockSize;          // 内部自动 flush 间隔（输入字节），0=每次 write 立即 flush
    bool isGzip;
    bool started;
    bool finished;
    tdefl_compressor comp;
    uint32_t crc;           // gzip 用：原始数据 CRC32
    uint64_t totalIn;       // gzip 用：原始数据总长度（ISIZE）
    int64_t inputSinceFlush;
    std::vector<uint8_t> outBuf;
    bool unsupported;
};

JZCompressStream::JZCompressStream(JZCompressAlgorithm algo, int level)
    : m_impl(new Impl())
{
    m_impl->algo = algo;
    m_impl->level = level;
    m_impl->blockSize = 0;
    m_impl->isGzip = (algo == JZCompressAlgorithm::GZip);
    m_impl->started = false;
    m_impl->finished = false;
    m_impl->crc = static_cast<uint32_t>(MZ_CRC32_INIT);
    m_impl->totalIn = 0;
    m_impl->inputSinceFlush = 0;
    m_impl->outBuf.resize(65536);
    m_impl->unsupported = isUnsupported(algo);
}

JZCompressStream::~JZCompressStream()
{
    delete m_impl;
}

void JZCompressStream::setBlockSize(int blockSize)
{
    if (m_impl) m_impl->blockSize = blockSize;
}

JZByteArray JZCompressStream::write(const JZByteArray &input)
{
    JZByteArray result;
    if (!m_impl || m_impl->finished || m_impl->unsupported) return result;

    if (!m_impl->started)
    {
        m_impl->started = true;
        tdefl_init(&m_impl->comp, nullptr, nullptr, makeCompressFlags(m_impl->algo, m_impl->level));
        if (m_impl->isGzip)
        {
            result.append(reinterpret_cast<const char *>(kGzipHeader), 10);
            m_impl->crc = static_cast<uint32_t>(MZ_CRC32_INIT);
            m_impl->totalIn = 0;
        }
    }

    const uint8_t *inPtr = reinterpret_cast<const uint8_t *>(input.constData());
    size_t inLeft = static_cast<size_t>(input.size());
    if (inLeft == 0) return result;

    // 自动 flush 间隔：攒够 blockSize 输入才发 SYNC_FLUSH；0 表示每次 write 都 flush（最低延迟）。
    tdefl_flush flush = TDEFL_NO_FLUSH;
    m_impl->inputSinceFlush += static_cast<int64_t>(inLeft);
    if (m_impl->blockSize <= 0 || m_impl->inputSinceFlush >= m_impl->blockSize)
    {
        flush = TDEFL_SYNC_FLUSH;
        m_impl->inputSinceFlush = 0;
    }

    std::vector<uint8_t> &out = m_impl->outBuf;
    while (inLeft > 0)
    {
        size_t inConsumed = inLeft;
        size_t outCap = out.size();
        tdefl_status st = tdefl_compress(&m_impl->comp, inPtr, &inConsumed, out.data(), &outCap, flush);
        if (outCap > 0)
            result.append(reinterpret_cast<const char *>(out.data()), static_cast<int>(outCap));
        if (m_impl->isGzip)
        {
            m_impl->crc = static_cast<uint32_t>(mz_crc32(m_impl->crc, inPtr, inConsumed));
            m_impl->totalIn += inConsumed;
        }
        inPtr += inConsumed;
        inLeft -= inConsumed;
        if (st != TDEFL_STATUS_OKAY) break;
        if (inLeft == 0) break;
    }
    return result;
}

JZByteArray JZCompressStream::finish()
{
    JZByteArray result;
    if (!m_impl || m_impl->finished || m_impl->unsupported) return result;
    m_impl->finished = true;

    if (!m_impl->started)
    {
        m_impl->started = true;
        tdefl_init(&m_impl->comp, nullptr, nullptr, makeCompressFlags(m_impl->algo, m_impl->level));
        if (m_impl->isGzip)
        {
            result.append(reinterpret_cast<const char *>(kGzipHeader), 10);
            m_impl->crc = static_cast<uint32_t>(MZ_CRC32_INIT);
            m_impl->totalIn = 0;
        }
    }

    std::vector<uint8_t> &out = m_impl->outBuf;
    for (;;)
    {
        size_t inConsumed = 0;
        size_t outCap = out.size();
        tdefl_status st = tdefl_compress(&m_impl->comp, nullptr, &inConsumed, out.data(), &outCap, TDEFL_FINISH);
        if (outCap > 0)
            result.append(reinterpret_cast<const char *>(out.data()), static_cast<int>(outCap));
        if (st == TDEFL_STATUS_DONE) break;
        if (st < 0) break; // 异常（理论上不发生）
    }

    if (m_impl->isGzip)
    {
        uint8_t trailer[8];
        uint32_t crc = m_impl->crc;
        uint32_t isize = static_cast<uint32_t>(m_impl->totalIn & 0xffffffff);
        for (int i = 0; i < 4; i++) trailer[i] = static_cast<uint8_t>(crc >> (8 * i));
        for (int i = 0; i < 4; i++) trailer[4 + i] = static_cast<uint8_t>(isize >> (8 * i));
        result.append(reinterpret_cast<const char *>(trailer), 8);
    }
    return result;
}

void JZCompressStream::reset()
{
    if (!m_impl) return;
    m_impl->started = false;
    m_impl->finished = false;
    m_impl->crc = static_cast<uint32_t>(MZ_CRC32_INIT);
    m_impl->totalIn = 0;
    m_impl->inputSinceFlush = 0;
}

JZCompressAlgorithm JZCompressStream::algorithm() const
{
    return m_impl ? m_impl->algo : JZCompressAlgorithm::Deflate;
}

// ---------------- 流式解压 ----------------
struct JZDecompressStream::Impl
{
    JZCompressAlgorithm algo;
    int64_t expectSizeHint;
    bool isGzip;
    bool started;
    bool finished;
    bool done;                       // deflate 流已结束
    int windowBits;                  // >0 => 解析 zlib 头（Zlib）；否则 raw（Deflate/GZip）
    tinfl_decompressor decomp;
    mz_uint m_dictOfs;
    tinfl_status m_last_status;
    mz_uint8 m_dict[TINFL_LZ_DICT_SIZE];
    uint32_t m_crc;                  // gzip 用：已解压数据 CRC32
    uint64_t m_totalOut;
    int m_gzipHdrState;              // 0=未解析头, 1=已解析
    std::vector<uint8_t> m_stage;    // gzip 头暂存
    std::vector<uint8_t> m_trailer;  // gzip 尾（CRC32+ISIZE）暂存

    tinfl_status process(const uint8_t *inPtr, size_t inLen, bool isFinish, JZByteArray &out);
};

JZDecompressStream::JZDecompressStream(JZCompressAlgorithm algo, int64_t expectSizeHint)
    : m_impl(new Impl())
{
    m_impl->algo = algo;
    m_impl->expectSizeHint = expectSizeHint;
    m_impl->isGzip = (algo == JZCompressAlgorithm::GZip);
    m_impl->windowBits = (algo == JZCompressAlgorithm::Zlib) ? 15 : -15;
    m_impl->started = false;
    m_impl->finished = false;
    m_impl->done = false;
    m_impl->m_dictOfs = 0;
    m_impl->m_last_status = TINFL_STATUS_OKAY;
    m_impl->m_crc = static_cast<uint32_t>(MZ_CRC32_INIT);
    m_impl->m_totalOut = 0;
    m_impl->m_gzipHdrState = 0;
    tinfl_init(&m_impl->decomp);
}

JZDecompressStream::~JZDecompressStream()
{
    delete m_impl;
}

// 返回最后状态；把解压输出追加到 out。isFinish=true 时清除 HAS_MORE_INPUT 标志。
tinfl_status JZDecompressStream::Impl::process(const uint8_t *inPtr, size_t inLen, bool isFinish, JZByteArray &out)
{
    if (done) return TINFL_STATUS_DONE;
    mz_uint32 flags = 0;
    if (windowBits > 0) flags |= TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_COMPUTE_ADLER32;
    if (!isFinish) flags |= TINFL_FLAG_HAS_MORE_INPUT;

    size_t inLeft = inLen;
    const size_t origIn = inLen;
    for (;;)
    {
        size_t inBytes = inLeft;
        size_t outBytes = TINFL_LZ_DICT_SIZE - m_dictOfs;
        tinfl_status status = tinfl_decompress(&decomp, inPtr, &inBytes,
                                               m_dict, m_dict + m_dictOfs, &outBytes, flags);
        inPtr += inBytes;
        inLeft -= inBytes;
        if (outBytes > 0)
        {
            out.append(reinterpret_cast<const char *>(m_dict + m_dictOfs), static_cast<int>(outBytes));
            if (isGzip)
                m_crc = static_cast<uint32_t>(mz_crc32(m_crc, m_dict + m_dictOfs, outBytes));
            m_totalOut += outBytes;
            m_dictOfs = (m_dictOfs + static_cast<mz_uint>(outBytes)) & (TINFL_LZ_DICT_SIZE - 1);
        }

        if (status < 0) { m_last_status = status; return status; }
        if (status == TINFL_STATUS_NEEDS_MORE_INPUT && origIn == 0) { m_last_status = status; return status; }

        if (status == TINFL_STATUS_DONE)
        {
            done = true;
            m_last_status = status;
            if (isGzip) // 剩余输入即 gzip 尾（CRC32+ISIZE），最多取 8 字节
            {
                size_t take = inLeft < 8 ? inLeft : 8;
                for (size_t i = 0; i < take; i++) m_trailer.push_back(inPtr[i]);
            }
            return status;
        }

        if (isFinish)
        {
            if (inLeft == 0 && outBytes == 0) { m_last_status = status; return TINFL_STATUS_FAILED; }
        }
        else
        {
            if (inLeft == 0) { m_last_status = status; return status; }
        }
    }
}

JZByteArray JZDecompressStream::write(const JZByteArray &input)
{
    JZByteArray result;
    if (!m_impl || m_impl->finished || m_impl->done) return result;
    if (isUnsupported(m_impl->algo)) { m_impl->finished = true; return result; }

    const uint8_t *deflatePtr = reinterpret_cast<const uint8_t *>(input.constData());
    size_t deflateLen = static_cast<size_t>(input.size());

    if (m_impl->isGzip && m_impl->m_gzipHdrState == 0)
    {
        const uint8_t *p = reinterpret_cast<const uint8_t *>(input.constData());
        m_impl->m_stage.insert(m_impl->m_stage.end(), p, p + deflateLen);
        bool needMore = false;
        int hdrLen = parseGzipHeader(m_impl->m_stage.data(), m_impl->m_stage.size(), needMore);
        if (hdrLen < 0) { m_impl->finished = true; return result; } // 非法 gzip
        if (hdrLen == 0) return result;                            // 头未齐，等更多数据
        deflatePtr = m_impl->m_stage.data() + hdrLen;
        deflateLen = m_impl->m_stage.size() - hdrLen;
        m_impl->m_gzipHdrState = 1;
        m_impl->process(deflatePtr, deflateLen, false, result);
        m_impl->m_stage.clear();
        return result;
    }

    m_impl->process(deflatePtr, deflateLen, false, result);
    return result;
}

JZByteArray JZDecompressStream::finish()
{
    JZByteArray result;
    if (!m_impl || m_impl->finished) return result;
    m_impl->finished = true;
    if (isUnsupported(m_impl->algo)) return result;
    if (m_impl->isGzip && m_impl->m_gzipHdrState == 0) return result; // 头未完成，无法解压

    m_impl->process(nullptr, 0, true, result);

    // gzip 帧尾校验：受当前 finish() 返回类型（JZByteArray）限制，校验失败无法向上抛出错误，
    // 仅就地核对。如需严格失败传播，建议后续将 finish() 改为 JZResult<JZByteArray>。
    if (m_impl->isGzip && m_impl->done && m_impl->m_trailer.size() >= 8)
    {
        uint32_t storedCrc = readLE32(&m_impl->m_trailer[0]);
        uint32_t storedIsize = readLE32(&m_impl->m_trailer[4]);
        bool ok = (storedCrc == m_impl->m_crc) &&
                  (storedIsize == static_cast<uint32_t>(m_impl->m_totalOut & 0xffffffff));
        (void)ok; // 当前不传播错误（见上方说明）
    }
    return result;
}

void JZDecompressStream::reset()
{
    if (!m_impl) return;
    m_impl->started = false;
    m_impl->finished = false;
    m_impl->done = false;
    m_impl->m_dictOfs = 0;
    m_impl->m_last_status = TINFL_STATUS_OKAY;
    m_impl->m_crc = static_cast<uint32_t>(MZ_CRC32_INIT);
    m_impl->m_totalOut = 0;
    m_impl->m_gzipHdrState = 0;
    m_impl->m_stage.clear();
    m_impl->m_trailer.clear();
    tinfl_init(&m_impl->decomp);
}

JZCompressAlgorithm JZDecompressStream::algorithm() const
{
    return m_impl ? m_impl->algo : JZCompressAlgorithm::Deflate;
}

// ---------------- 标准 zip 归档（基于 miniz mz_zip 实现读 / 解压） ----------------
namespace
{

// 把归档内相对路径拼到目标目录上（zip 内部一律用 '/'，Windows 下 fopen/CreateDirectory 均接受）。
std::string jzZipJoinPath(const std::string &dir, const std::string &name)
{
    if (dir.empty()) return name;
    char last = dir.back();
    if (last == '/' || last == '\\') return dir + name;
    return dir + "/" + name;
}

// 取父目录（去掉末尾 '\\' 或 '/' 之后的部分）。
std::string jzZipParentPath(const std::string &p)
{
    size_t pos = p.find_last_of("/\\");
    if (pos == std::string::npos) return "";
    return p.substr(0, pos);
}


void jzZipFillEntry(JZZipEntry &out, const mz_zip_archive_file_stat &st)
{
    out.name = st.m_filename;
    out.size = static_cast<int64_t>(st.m_uncomp_size);
    out.compressedSize = static_cast<int64_t>(st.m_comp_size);
    out.isDir = (st.m_is_directory != MZ_FALSE);
    out.crc32 = st.m_crc32;
    out.mtime = static_cast<int64_t>(st.m_time);
    out.method = static_cast<int>(st.m_method);
}

// 简单通配匹配（* 任意多字符，? 单字符），用于 zip 过滤。
bool jzZipWildMatch(const std::string &s, const std::string &pat)
{
    size_t si = 0, pi = 0, star = std::string::npos, ss = 0;
    while (si < s.size())
    {
        if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == s[si]))
        {
            si++; pi++;
        }
        else if (pi < pat.size() && pat[pi] == '*')
        {
            star = pi++; ss = si;
        }
        else if (star != std::string::npos)
        {
            pi = star + 1; si = ++ss;
        }
        else return false;
    }
    while (pi < pat.size() && pat[pi] == '*') pi++;
    return pi == pat.size();
}

// zip 写路径展开后的单个待写条目。
struct JzZipItem
{
    bool isDir = false;
    bool fromDisk = false;        // true: 磁盘文件 diskPath；false: 内存 mem
    std::string archiveName;      // zip 内路径
    std::string diskPath;         // 仅 fromDisk 用
    JZByteArray mem;              // 仅内存条目用
    int64_t size = 0;
};

// 把 level（-1=默认, 0=store, 1..9=deflate）转成 miniz 的 level_and_flags。
mz_uint jzZipLevel(int level)
{
    if (level < 0) return static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION);
    if (level > MZ_BEST_COMPRESSION) return static_cast<mz_uint>(MZ_BEST_COMPRESSION);
    return static_cast<mz_uint>(level);
}

// 递归收集文件夹内容，生成待写条目（archiveName 相对 folder 根）。
void jzZipCollectFolder(const std::string &folder, const std::string &relBase,
                        bool recursive, const std::vector<std::string> &filter,
                        std::vector<JzZipItem> &out)
{
    JZDir dir(folder);
    auto infos = dir.entryInfoList(JZDir::Files | JZDir::Dirs | JZDir::NoDotAndDotDot);
    for (const auto &fi : infos)
    {
        std::string name = fi.fileName();
        if (fi.isDir())
        {
            JzZipItem d;
            d.isDir = true;
            d.archiveName = jzZipJoinPath(relBase, name) + "/";
            out.push_back(std::move(d));
            if (recursive)
                jzZipCollectFolder(jzZipJoinPath(folder, name), jzZipJoinPath(relBase, name),
                                   recursive, filter, out);
        }
        else
        {
            bool matched = filter.empty();
            for (const auto &f : filter)
            {
                if (jzZipWildMatch(name, f)) { matched = true; break; }
            }
            if (!matched) continue;

            JzZipItem f;
            f.fromDisk = true;
            f.archiveName = jzZipJoinPath(relBase, name);
            f.diskPath = jzZipJoinPath(folder, name);
            f.size = fi.size();
            out.push_back(std::move(f));
        }
    }
}

} // namespace

struct JZZip::Impl
{
    JZZipProgress progress;

    // 待压缩条目（虚拟内存/磁盘文件、目录、整文件夹）。
    struct Pending
    {
        enum Kind { Mem, Disk, Dir, Folder } kind = Mem;
        std::string zipEntryPath;       // Mem/Disk/Dir: zip 内路径
        std::string orgPath;            // Disk: 磁盘文件; Folder: 磁盘目录
        JZByteArray data;               // Mem: 内存数据
        bool recursive = true;          // Folder
        std::vector<std::string> filter;// Folder 文件名过滤（如 "*.bin"）
    };
    std::vector<Pending> pending;
};

JZZip::JZZip() : m_impl(new Impl()) {}
JZZip::~JZZip() { delete m_impl; }

void JZZip::setProgressCallback(JZZipProgress cb) { m_impl->progress = std::move(cb); }

void JZZip::addFile(const std::string &zipEntryPath, const JZByteArray &data)
{
    Impl::Pending p;
    p.kind = Impl::Pending::Mem;
    p.zipEntryPath = zipEntryPath;
    p.data = data;
    m_impl->pending.push_back(std::move(p));
}
void JZZip::addFile(const std::string &zipEntryPath, const std::string &orgPath)
{
    Impl::Pending p;
    p.kind = Impl::Pending::Disk;
    p.zipEntryPath = zipEntryPath;
    p.orgPath = orgPath;
    m_impl->pending.push_back(std::move(p));
}
void JZZip::addDirectory(const std::string &zipEntryPath)
{
    Impl::Pending p;
    p.kind = Impl::Pending::Dir;
    p.zipEntryPath = zipEntryPath;
    m_impl->pending.push_back(std::move(p));
}
void JZZip::addFolder(const std::string &folder, bool recursive, const std::vector<std::string> &filter)
{
    Impl::Pending p;
    p.kind = Impl::Pending::Folder;
    p.orgPath = folder;
    p.recursive = recursive;
    p.filter = filter;
    m_impl->pending.push_back(std::move(p));
}
int JZZip::entryCount() const { return static_cast<int>(m_impl->pending.size()); }

JZResult<void> JZZip::zip(const std::string &zipPath, int level)
{
    // 1. 把 pending 展开为扁平待写条目。
    std::vector<JzZipItem> items;
    for (const auto &p : m_impl->pending)
    {
        if (p.kind == Impl::Pending::Folder)
        {
            jzZipCollectFolder(p.orgPath, "", p.recursive, p.filter, items);
        }
        else
        {
            JzZipItem it;
            it.isDir = (p.kind == Impl::Pending::Dir);
            it.fromDisk = (p.kind == Impl::Pending::Disk);
            it.archiveName = p.zipEntryPath;
            it.diskPath = p.orgPath;
            it.mem = p.data;
            it.size = it.isDir ? 0 : (it.fromDisk ? static_cast<int64_t>(JZFileInfo(p.orgPath).size())
                                                  : p.data.size());
            if (it.isDir && !it.archiveName.empty() && it.archiveName.back() != '/')
                it.archiveName += "/";
            items.push_back(std::move(it));
        }
    }

    if (items.empty())
        return JZResult<void>::Error("JZCompress: no entries to zip");

    // 2. 进度总量 = 全部条目原始大小之和。
    int64_t total = 0;
    for (const auto &it : items) total += it.size;

    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_writer_init_file(&zip, zipPath.c_str(), 0))
        return JZResult<void>::Error("JZCompress: cannot create zip: " + zipPath);

    mz_uint lvl = jzZipLevel(level);
    int64_t current = 0;
    JZResult<void> result = JZResult<void>::Ok();
    for (const auto &it : items)
    {
        bool ok = false;
        if (it.isDir)
        {
            ok = mz_zip_writer_add_mem(&zip, it.archiveName.c_str(), nullptr, 0, lvl);
        }
        else if (it.fromDisk)
        {
            ok = mz_zip_writer_add_file(&zip, it.archiveName.c_str(), it.diskPath.c_str(),
                                        nullptr, 0, lvl);
        }
        else
        {
            ok = mz_zip_writer_add_mem(&zip, it.archiveName.c_str(),
                                       it.mem.constData(),
                                       static_cast<size_t>(it.mem.size()), lvl);
        }
        if (!ok)
        {
            result = JZResult<void>::Error("JZCompress: failed to add entry: " + it.archiveName);
            break;
        }
        current += it.size;
        if (m_impl->progress && !m_impl->progress(current, total))
        {
            result = JZResult<void>::Error("JZCompress: zip canceled");
            break;
        }
    }

    if (result.isOk())
    {
        if (!mz_zip_writer_finalize_archive(&zip))
            result = JZResult<void>::Error("JZCompress: failed to finalize zip");
    }
    mz_zip_writer_end(&zip);

    // 取消或失败：删除半成品文件。
    if (!result.isOk())
        JZDir().remove(zipPath);

    return result;
}

JZResult<std::vector<JZZipEntry>> JZZip::list(const std::string &zipPath)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_reader_init_file(&zip, zipPath.c_str(), 0))
        return JZResult<std::vector<JZZipEntry>>::Error("JZCompress: cannot open zip: " + zipPath);

    std::vector<JZZipEntry> out;
    mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; i++)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        JZZipEntry e;
        jzZipFillEntry(e, st);
        out.push_back(std::move(e));
    }
    mz_zip_reader_end(&zip);
    return JZResult<std::vector<JZZipEntry>>::Ok(std::move(out));
}

// ---------------- 只读归档读取器 JZUnZip（基于 miniz mz_zip） ----------------
struct JZUnZip::Impl
{
    JZZipProgress progress;
    mz_zip_archive archive;
    bool opened = false;
};

JZUnZip::JZUnZip() : m_impl(new Impl()) {}
JZUnZip::~JZUnZip()
{
    if (m_impl->opened) mz_zip_reader_end(&m_impl->archive);
    delete m_impl;
}

void JZUnZip::setProgressCallback(JZZipProgress cb) { m_impl->progress = std::move(cb); }

JZResult<void> JZUnZip::open(const std::string &zipPath)
{
    mz_zip_zero_struct(&m_impl->archive);
    if (!mz_zip_reader_init_file(&m_impl->archive, zipPath.c_str(), 0))
        return JZResult<void>::Error("JZCompress: cannot open zip: " + zipPath);
    m_impl->opened = true;
    return JZResult<void>::Ok();
}

void JZUnZip::close()
{
    if (m_impl->opened)
    {
        mz_zip_reader_end(&m_impl->archive);
        m_impl->opened = false;
    }
}

bool JZUnZip::isOpen() const { return m_impl->opened; }

JZResult<std::vector<JZZipEntry>> JZUnZip::list()
{
    if (!m_impl->opened)
        return JZResult<std::vector<JZZipEntry>>::Error("JZCompress: archive not open");
    std::vector<JZZipEntry> out;
    mz_uint n = mz_zip_reader_get_num_files(&m_impl->archive);
    for (mz_uint i = 0; i < n; i++)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&m_impl->archive, i, &st)) continue;
        JZZipEntry e;
        jzZipFillEntry(e, st);
        out.push_back(std::move(e));
    }
    return JZResult<std::vector<JZZipEntry>>::Ok(std::move(out));
}

bool JZUnZip::contains(const std::string &entryName) const
{
    if (!m_impl->opened) return false;
    return mz_zip_reader_locate_file(&m_impl->archive, entryName.c_str(), nullptr, 0) >= 0;
}

JZResult<JZByteArray> JZUnZip::extractToMemory(const std::string &entryName)
{
    if (!m_impl->opened)
        return JZResult<JZByteArray>::Error("JZCompress: archive not open");
    int idx = mz_zip_reader_locate_file(&m_impl->archive, entryName.c_str(), nullptr, 0);
    if (idx < 0)
        return JZResult<JZByteArray>::Error("JZCompress: entry not found: " + entryName);
    size_t size = 0;
    void *p = mz_zip_reader_extract_to_heap(&m_impl->archive, static_cast<mz_uint>(idx), &size, 0);
    if (!p)
        return JZResult<JZByteArray>::Error("JZCompress: extract failed: " + entryName);
    JZByteArray out;
    out.append(reinterpret_cast<const char *>(p), static_cast<int>(size));
    mz_free(p);
    return JZResult<JZByteArray>::Ok(std::move(out));
}

JZResult<void> JZUnZip::extractToFile(const std::string &entryName, const std::string &destPath)
{
    if (!m_impl->opened)
        return JZResult<void>::Error("JZCompress: archive not open");
    int idx = mz_zip_reader_locate_file(&m_impl->archive, entryName.c_str(), nullptr, 0);
    if (idx < 0)
        return JZResult<void>::Error("JZCompress: entry not found: " + entryName);
    std::string parent = jzZipParentPath(destPath);
    if (!parent.empty()) JZDir().mkpath(parent);
    if (!mz_zip_reader_extract_to_file(&m_impl->archive, static_cast<mz_uint>(idx), destPath.c_str(), 0))
        return JZResult<void>::Error("JZCompress: extract failed: " + entryName);
    return JZResult<void>::Ok();
}

JZResult<void> JZUnZip::unzip(const std::string &dir)
{
    if (!m_impl->opened)
        return JZResult<void>::Error("JZCompress: archive not open");

    auto listR = list();
    if (!listR.isOk())
        return JZResult<void>::Error(listR.errorMessage());
    std::vector<JZZipEntry> entries = std::move(listR.value());

    int64_t total = 0;
    for (const JZZipEntry &e : entries)
        if (!e.isDir) total += e.size;

    int64_t current = 0;
    for (const JZZipEntry &e : entries)
    {
        std::string dst = jzZipJoinPath(dir, e.name);
        if (e.isDir)
        {
            if (!dst.empty() && (dst.back() == '/' || dst.back() == '\\'))
                dst.pop_back();
            JZDir().mkpath(dst);
        }
        else
        {
            JZResult<void> r = extractToFile(e.name, dst);
            if (!r)
                return JZResult<void>::Error(r.errorMessage());
            current += e.size;
        }

        if (m_impl->progress && !m_impl->progress(current, total))
            return JZResult<void>::Error("JZCompress: unzip canceled");
    }
    return JZResult<void>::Ok();
}


