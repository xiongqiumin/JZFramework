#include "JZHttpStaticFiles.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "JZDir.h"
#include "JZDateTime.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZHttpServer.h"
#include "JZStd.h"

// 解析 Range 头，返回:
//   0 - 无有效范围（按普通 200 返回全部）
//   1 - 有效单范围（start/end 已填充，[start,end] 闭区间，end 已夹紧到 fileSize-1）
//   2 - 范围不满足（应返回 416）
// 仅支持单范围；若请求多范围（bytes=a-b,c-d）取第一个，覆盖绝大多数浏览器/下载器场景。
static int parseRange(const std::string& header, int fileSize, long long& start, long long& end)
{
    if (header.size() < 6 || strncmp(header.c_str(), "bytes=", 6) != 0)
        return 0;
    std::string ranges = header.substr(6);

    // 多范围 (bytes=a-b,c-d): 本服务仅支持单范围，按 HTTP 规范回退为完整 200 响应
    // 不能返回 416 —— 416 语义是"范围不满足"而非"格式不支持"，会让客户端误判放弃下载
    size_t comma = ranges.find(',');
    if (comma != std::string::npos)
        return 0;

    size_t dash = ranges.find('-');
    if (dash == std::string::npos)
        return 0;

    std::string s_start = ranges.substr(0, dash);
    std::string s_end = ranges.substr(dash + 1);

    if (s_start.empty())
    {
        // 后缀范围: bytes=-N 表示最后 N 字节
        long long last = atoll(s_end.c_str());
        if (last <= 0)
            return 2;
        start = std::max(0LL, (long long)fileSize - last);
        end = (long long)fileSize - 1;
    }
    else
    {
        start = atoll(s_start.c_str());
        if (s_end.empty())
            end = (long long)fileSize - 1;
        else
            end = atoll(s_end.c_str());
    }

    if (start < 0 || end < start || start >= fileSize)
        return 2;
    if (end >= fileSize)
        end = (long long)fileSize - 1;   // 容忍 end 超出，夹紧到末尾
    return 1;
}

// 将 RFC 1123 / asctime 形式的 HTTP 日期解析为 epoch 毫秒。
// 注意: 与 JZHttpStaticFiles 内部 buildFileTime 一致，按"本地时间"解释
// (本模块 Last-Modified 实际以本地时间格式化却标 GMT，此处保持同样的解释以保证
//  If-Modified-Since 与文件 mtime 的可比性，避免引入时区偏移)。
static int httpMonthFromName(const char *m)
{
    static const char *kMonth[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i = 0; i < 12; i++)
    {
        if (str_toLower(m) == str_toLower(kMonth[i]))
            return i + 1;
    }
    return 0;
}

static int64_t parseHttpDate(const std::string &s)
{
    int day = 0, year = 0, hour = 0, min = 0, sec = 0;
    char mon[4] = {0};
    // RFC 1123: "Sun, 02 Sep 2026 12:00:00 GMT"
    if (sscanf(s.c_str(), "%*3s, %d %3s %d %d:%d:%d", &day, mon, &year, &hour, &min, &sec) == 6)
    {
        int m = httpMonthFromName(mon);
        if (m > 0)
            return JZDateTime(year, m, day, hour, min, sec).toMSecsSinceEpoch();
    }
    // asctime: "Sun Sep  2 12:00:00 2026"
    if (sscanf(s.c_str(), "%*3s %3s %d %d:%d:%d %d", mon, &day, &hour, &min, &sec, &year) == 6)
    {
        int m = httpMonthFromName(mon);
        if (m > 0)
            return JZDateTime(year, m, day, hour, min, sec).toMSecsSinceEpoch();
    }
    return -1;
}

JZHttpStaticFiles::JZHttpStaticFiles(const std::string& rootDir)
{
    // 以 ':' 开头的 rootDir 表示资源路径（如 ":/xxx"），否则为文件系统目录。
    // 是否资源路径直接由 rootDir 文件名判断，无需额外标志位。
    if (str_startsWith(rootDir, ":/"))
        m_absRoot = rootDir;
    else
        m_absRoot = JZFileInfo::weaklyCanonicalPath(JZDir(rootDir).absolutePath());

    setChunkSize(m_chunkSize);
}

std::string JZHttpStaticFiles::localPath(const std::string& userPath)
{
    std::string web_path = userPath;
    if (str_endsWith(userPath, "/"))
        web_path += "index.html";

    if (str_startsWith(m_absRoot, ":/"))
    {
        // 资源路径：去掉 : 前缀 → canonicalize → 加回 :
        std::string raw = m_absRoot + web_path;  // web_path 已带 /
        return ":" + JZFileInfo::weaklyCanonicalPath(raw.substr(1));
    }

    // 始终在根目录下解析：web_path 以 '/' 开头，直接拼到 m_absRoot 后规范化，
    // 避免 JZDir::absoluteFilePath 把以 '/' 开头的请求路径当作绝对路径而忽略根目录（导致 403）
    std::string path = m_absRoot + web_path;
    return JZFileInfo::weaklyCanonicalPath(path);
}

void JZHttpStaticFiles::handle(JZHttpServerClient* client, const JZHttpRequest& req)
{
    JZHttpRespone resp;
    std::string local_path = localPath(req.path());

    if (!str_startsWith(local_path, m_absRoot))
    {
        resp.setStatusCode(403);
        resp.setBody("403 Forbidden");
        client->response(resp.toByteArray());
        return;
    }

    bool isResource = str_startsWith(m_absRoot, ":/");

    // -- 缓存 & 304（文件系统路径，在打开文件流之前判断） --
    std::string etag, lastModified;
    int fileSize = 0;
    if (!isResource)
    {
        JZFileInfo fileInfo(local_path);
        if (!fileInfo.exists())
        {
            resp.setStatusCode(404);
            resp.setBody("404 Not Found");
            client->response(resp.toByteArray());
            return;
        }
        fileSize = fileInfo.size();

        etag = "W/\"" + std::to_string(fileSize) + "-" + std::to_string(fileInfo.lastModified()) + "\"";
        JZDateTime dt = JZDateTime::fromMSecsSinceEpoch(fileInfo.lastModified());
        static const char* kWeekDay[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        static const char* kMonth[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        char buf[48];
        snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d GMT",
                 kWeekDay[dt.weekDay()], dt.day(), kMonth[dt.month() - 1],
                 dt.year(), dt.hour(), dt.minute(), dt.second());
        lastModified = buf;

        // 304 Not Modified：匹配则直接返回，不打开文件流
        const std::string& ifNoneMatch = req.header("If-None-Match");
        const std::string& ifModSince = req.header("If-Modified-Since");
        // 按时间比较而非字符串精确相等：容忍客户端日期格式/精度差异
        // (文件自给定时间后未被修改 -> 304)
        int64_t ifModTime = parseHttpDate(ifModSince);
        if ((!etag.empty() && ifNoneMatch == etag) ||
            (!lastModified.empty() && ifModTime >= 0 && fileInfo.lastModified() <= ifModTime))
        {
            resp.setStatusCode(304);
            resp.setHeader("Cache-Control", "public, max-age=" + std::to_string(m_cacheMaxAge));
            resp.setHeader("ETag", etag);
            resp.setHeader("Last-Modified", lastModified);
            client->response(resp.toByteArray());
            return;
        }
    }

    // 打开文件流
    if (!openStreamFile(client, local_path))
    {
        resp.setStatusCode(404);
        resp.setBody("404 Not Found");
        client->response(resp.toByteArray());
        return;
    }

    StreamState& st = m_streams[client];
    if (isResource)
        fileSize = st.remainSize;   // 资源路径没有提前 stat，从流状态获取
    resp.setHeader("Content-Type", getContentType(local_path));
    resp.setHeader("Accept-Ranges", "bytes");

    if (isResource)
        resp.setHeader("Cache-Control", "public, max-age=31536000, immutable");
    else
    {
        resp.setHeader("Cache-Control", "public, max-age=" + std::to_string(m_cacheMaxAge));
        resp.setHeader("ETag", etag);
        resp.setHeader("Last-Modified", lastModified);
    }

    long long rstart = 0, rend = 0;
    int rc = parseRange(req.header("Range"), fileSize, rstart, rend);
    if (rc == 2)
    {
        resp.setStatusCode(416);
        resp.setHeader("Content-Range", "bytes */" + std::to_string(fileSize));
        client->response(resp.toByteArray());
        closeStreamFile(client);
        return;
    }
    if (rc == 1)
    {
        resp.setStatusCode(206);
        resp.setHeader("Content-Range",
            "bytes " + std::to_string(rstart) + "-" + std::to_string(rend) + "/" + std::to_string(fileSize));
        st.file.seek((int)rstart, SEEK_SET);
        st.remainSize = (int)(rend - rstart + 1);
    }

    if (req.methodString() == "HEAD")
    {
        JZByteArray head = resp.toHeadByteArray(st.remainSize);
        client->response(head);
        closeStreamFile(client);
        return;
    }

    JZByteArray head = resp.toHeadByteArray(st.remainSize);
    client->write(head, false);
    st.pendingSize += head.size();
    writeStreamFile(client);
}

void JZHttpStaticFiles::onNetEvent(JZHttpServerClient* client, JZNetResEvent *event)
{
    auto it = m_streams.find(client);
    if (it == m_streams.end())
        return;

    if (event->event == JZ_NET_EV_WRITE)
    {
        // 上一次写入已落 socket，继续写下一个块；读完则 is_finish=true 结束流式响应
        JZDataStream s(&event->binData);
        int32_t write_size = 0;
        s >> write_size;

        it->second.pendingSize -= write_size;
        if (it->second.pendingSize == 0)
            writeStreamFile(client);
    }
}

bool JZHttpStaticFiles::openStreamFile(JZHttpServerClient* client, const std::string& path)
{
    StreamState& st = m_streams[client];   // 默认构造（file 关闭、size 0），避免 JZFile 不可拷贝/移动
    if (!st.file.open(path, "rb"))
    {
        m_streams.erase(client);
        return false;
    }
    client->setHandle(this);
    
    JZObjectPointer self = guardPointer();
    st.sigDestroyedId = connect(&client->sigDestroyed, this, [this, client](JZObject*) {
        closeStreamFile(client);
    });

    st.remainSize = st.file.size();
    return true;
}

void JZHttpStaticFiles::writeStreamFile(JZHttpServerClient* client)
{
    auto it = m_streams.find(client);
    if (it == m_streams.end())
        return;

    int size = it->second.file.read(m_streamBuffer.data(), m_streamBuffer.size());
    it->second.pendingSize += size;
    it->second.remainSize -= size;
    bool is_last = (it->second.remainSize == 0);
    client->write(m_streamBuffer.data(), size, is_last);
    if (is_last)
        closeStreamFile(client);   // 流式发送结束，清理该连接状态
}

void JZHttpStaticFiles::closeStreamFile(JZHttpServerClient* client)
{
    auto it = m_streams.find(client);
    if (it == m_streams.end())
        return;
    if (it->second.sigDestroyedId >= 0)
        client->sigDestroyed.removeById(it->second.sigDestroyedId);
    m_streams.erase(client);
}

std::string JZHttpStaticFiles::getContentType(const std::string& path)
{
    size_t pos = path.find_last_of('.');
    if (pos == std::string::npos)
        return "application/octet-stream";

    std::string ext = path.substr(pos);
    // 文本
    if (ext == ".html" || ext == ".htm")   return "text/html; charset=utf-8";
    if (ext == ".css")  return "text/css; charset=utf-8";
    if (ext == ".js" || ext == ".mjs")  return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".xml")  return "application/xml; charset=utf-8";
    // 图片
    if (ext == ".png")  return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".ico")  return "image/x-icon";
    if (ext == ".gif")  return "image/gif";
    if (ext == ".svg")  return "image/svg+xml";
    // 字体
    if (ext == ".woff")  return "font/woff";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".ttf")   return "font/ttf";
    // 媒体
    if (ext == ".mp4")  return "video/mp4";
    if (ext == ".webm") return "video/webm";
    // 其他
    if (ext == ".wasm") return "application/wasm";
    if (ext == ".pdf")  return "application/pdf";
    if (ext == ".zip")  return "application/zip";

    return "application/octet-stream";
}
