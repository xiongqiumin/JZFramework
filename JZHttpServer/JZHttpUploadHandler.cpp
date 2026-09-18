#include "JZHttpUploadHandler.h"

#include "JZHttpServer.h"    // JZHttpServerClient 完整定义
#include "JZHttpRespone.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZDir.h"
#include "JZCrypto.h"
#include "JZStd.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <string>

namespace {
// 去掉目录成分 / ".." / 非法字符, 防路径穿越(客户端的 name/key 不可直接当路径用)
std::string sanitizeName(const std::string& name)
{
    std::string s = name;
    size_t p = s.find_last_of("/\\");
    if (p != std::string::npos) s = s.substr(p + 1);
    size_t d;
    while ((d = s.find("..")) != std::string::npos) s.erase(d, 2);
    std::string out;
    for (char c : s)
    {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            continue;
        if ((unsigned char)c < 0x20) continue;
        out += c;
    }
    size_t i = 0;
    while (i < out.size() && out[i] == '.') i++;
    out = out.substr(i);
    if (out.empty()) out = "upload";
    return out;
}
}

JZHttpUploadHandler::JZHttpUploadHandler() = default;
JZHttpUploadHandler::~JZHttpUploadHandler() = default;

void JZHttpUploadHandler::setUploadDir(const std::string& dir)
{
    m_uploadDir = dir;
}

void JZHttpUploadHandler::setWriteMd5Sidecar(bool on)
{
    m_writeMd5 = on;
}

void JZHttpUploadHandler::handle(JZHttpServerClient* client, const JZHttpRequest& req)
{
    std::string key = req.param("key");
    if (key.empty())
    {
        sendJson(client, req, 400, "{\"status\":\"error\",\"msg\":\"missing route param <key>\"}");
        return;
    }

    const auto& qp = req.queryParams();
    bool init     = qp.find("init") != qp.end();
    bool complete = qp.find("complete") != qp.end();
    bool status   = qp.find("status") != qp.end() || req.method() == JZHttpRequest::GET;

    if (init)          handleInit(client, req, key);
    else if (complete) handleComplete(client, req, key);
    else if (status)   handleStatus(client, req, key);
    else               handleChunk(client, req, key);
}

void JZHttpUploadHandler::handleInit(JZHttpServerClient* client, const JZHttpRequest& req, const std::string& key)
{
    JZMutexLocker lock(&m_mutex);

    Session s;
    std::string name = req.queryParam("name");
    s.finalName = sanitizeName(name.empty() ? key : name);

    std::string sizeStr = req.queryParam("size");
    if (!sizeStr.empty())
    {
        try { s.expectedSize = std::stoll(sizeStr); } catch (...) { s.expectedSize = -1; }
    }
    s.expectedMd5 = req.queryParam("md5");   // hex

    JZDir().mkpath(m_uploadDir);
    s.partPath = m_uploadDir + "/" + sanitizeName(key) + ".part";

    // 创建 / 截断临时文件（重新开始一次上传）
    {
        JZFile f;
        if (!f.open(s.partPath, "wb+"))
        {
            sendJson(client, req, 500, "{\"status\":\"error\",\"msg\":\"cannot create part file\"}");
            return;
        }
    }

    m_sessions[key] = std::move(s);
    std::string json = "{\"status\":\"ok\",\"key\":\"" + jsonEscape(key) +
                       "\",\"expectedSize\":" + std::to_string(m_sessions[key].expectedSize) + "}";
    sendJson(client, req, 200, json);
}

void JZHttpUploadHandler::handleChunk(JZHttpServerClient* client, const JZHttpRequest& req, const std::string& key)
{
    JZMutexLocker lock(&m_mutex);

    auto it = m_sessions.find(key);
    if (it == m_sessions.end())
    {
        sendJson(client, req, 400, "{\"status\":\"error\",\"msg\":\"no session, call init first\"}");
        return;
    }
    Session& s = it->second;

    int64_t offset = 0;
    std::string offStr = req.queryParam("offset");
    if (!offStr.empty())
    {
        try { offset = std::stoll(offStr); } catch (...) { offset = -1; }
    }
    if (offset < 0)
    {
        sendJson(client, req, 400, "{\"status\":\"error\",\"msg\":\"invalid offset\"}");
        return;
    }

    const JZByteArray& body = req.body();
    if (body.size() == 0)
    {
        sendJson(client, req, 400, "{\"status\":\"error\",\"msg\":\"empty body\"}");
        return;
    }

    // 可选分块校验: crc 参数带上本块 CRC32(hex)
    std::string crcStr = req.queryParam("crc");
    if (!crcStr.empty())
    {
        uint32_t got = crc32(body.constData(), body.size());
        uint32_t exp = static_cast<uint32_t>(std::strtoul(crcStr.c_str(), nullptr, 16));
        if (got != exp)
        {
            sendJson(client, req, 400, "{\"status\":\"error\",\"msg\":\"chunk crc mismatch\"}");
            return;
        }
    }

    // 打开临时文件(不存在则创建), 随机写入到 offset
    JZFile f;
    if (JZFile::exists(s.partPath))
    {
        if (!f.open(s.partPath, "rb+"))
        {
            sendJson(client, req, 500, "{\"status\":\"error\",\"msg\":\"open part failed\"}");
            return;
        }
    }
    else
    {
        if (!f.open(s.partPath, "wb+"))
        {
            sendJson(client, req, 500, "{\"status\":\"error\",\"msg\":\"open part failed\"}");
            return;
        }
    }

    f.seek(static_cast<int>(offset));
    int w = f.write(body);
    f.close();
    if (w != static_cast<int>(body.size()))
    {
        sendJson(client, req, 500, "{\"status\":\"error\",\"msg\":\"write incomplete\"}");
        return;
    }

    mergeRange(s, offset, offset + static_cast<int64_t>(body.size()));
    std::string json = "{\"status\":\"ok\",\"offset\":" + std::to_string(offset) +
                       ",\"size\":" + std::to_string(body.size()) +
                       ",\"received\":" + std::to_string(receivedBytes(s)) + "}";
    sendJson(client, req, 200, json);
}

void JZHttpUploadHandler::handleComplete(JZHttpServerClient* client, const JZHttpRequest& req, const std::string& key)
{
    JZMutexLocker lock(&m_mutex);

    auto it = m_sessions.find(key);
    if (it == m_sessions.end())
    {
        sendJson(client, req, 400, "{\"status\":\"error\",\"msg\":\"no session\"}");
        return;
    }
    Session& s = it->second;

    int64_t actual = JZFileInfo(s.partPath).size();
    if (s.expectedSize >= 0 && actual != s.expectedSize)
    {
        sendJson(client, req, 400, "{\"status\":\"error\",\"msg\":\"size mismatch\"}");
        return;
    }
    if (!rangesCover(s, actual))
    {
        std::string json = "{\"status\":\"error\",\"msg\":\"missing chunks\",\"missing\":" +
                           missingRangesJson(s, actual) + "}";
        sendJson(client, req, 400, json);
        return;
    }

    // 整文件 MD5 校验
    JZByteArray whole;
    {
        JZFile f;
        if (!f.open(s.partPath, "rb"))
        {
            sendJson(client, req, 500, "{\"status\":\"error\",\"msg\":\"open part failed\"}");
            return;
        }
        whole = f.readAll();
    }
    std::string hex = toHex(JZCrypto::hash(JZCrypto::HashAlgorithm::MD5, whole));
    if (!s.expectedMd5.empty() && hex != s.expectedMd5)
    {
        sendJson(client, req, 400, "{\"status\":\"error\",\"msg\":\"md5 mismatch\"}");
        return;
    }

    // 落最终文件
    std::string finalPath = m_uploadDir + "/" + s.finalName;
    if (!JZFile::rename(s.partPath, finalPath))
    {
        sendJson(client, req, 500, "{\"status\":\"error\",\"msg\":\"rename failed\"}");
        return;
    }

    // 写出 <文件名>.md5 校验和侧车
    if (m_writeMd5)
    {
        JZFile mf;
        if (mf.open(finalPath + ".md5", "wb"))
            mf.write(hex.c_str(), (int)hex.size());   // JZFile::write 只收 JZByteArray / (char*, int)
    }

    m_sessions.erase(it);
    std::string json = "{\"status\":\"ok\",\"md5\":\"" + hex +
                       "\",\"size\":" + std::to_string(actual) +
                       ",\"path\":\"" + jsonEscape(finalPath) + "\"}";
    sendJson(client, req, 200, json);
}

void JZHttpUploadHandler::handleStatus(JZHttpServerClient* client, const JZHttpRequest& req, const std::string& key)
{
    JZMutexLocker lock(&m_mutex);

    auto it = m_sessions.find(key);
    if (it == m_sessions.end())
    {
        sendJson(client, req, 404, "{\"status\":\"error\",\"msg\":\"no session\"}");
        return;
    }
    const Session& s = it->second;
    int64_t actual = JZFile::exists(s.partPath) ? JZFileInfo(s.partPath).size() : 0;
    int64_t total  = s.expectedSize >= 0 ? s.expectedSize : actual;

    std::string json = "{\"status\":\"ok\",\"key\":\"" + jsonEscape(key) +
                       "\",\"received\":" + std::to_string(receivedBytes(s)) +
                       ",\"expected\":" + std::to_string(total) +
                       ",\"missing\":" + missingRangesJson(s, total) + "}";
    sendJson(client, req, 200, json);
}

// ---------------- 静态工具 ----------------

void JZHttpUploadHandler::mergeRange(Session& s, int64_t start, int64_t end)
{
    if (end <= start) return;
    auto& v = s.ranges;
    v.emplace_back(start, end);
    std::sort(v.begin(), v.end(), [](const std::pair<int64_t,int64_t>& a,
                                     const std::pair<int64_t,int64_t>& b) { return a.first < b.first; });

    std::vector<std::pair<int64_t,int64_t>> merged;
    for (const auto& p : v)
    {
        if (merged.empty()) merged.push_back(p);
        else
        {
            auto& last = merged.back();
            if (p.first <= last.second)                 // 重叠或相邻 -> 合并
                last.second = std::max(last.second, p.second);
            else
                merged.push_back(p);
        }
    }
    s.ranges = std::move(merged);
}

int64_t JZHttpUploadHandler::receivedBytes(const Session& s)
{
    int64_t n = 0;
    for (const auto& p : s.ranges) n += (p.second - p.first);
    return n;
}

bool JZHttpUploadHandler::rangesCover(const Session& s, int64_t total)
{
    if (s.ranges.empty()) return total == 0;
    int64_t cur = 0;
    for (const auto& p : s.ranges)
    {
        if (p.first > cur) return false;   // 中间有空洞
        cur = p.second;
    }
    return cur >= total;
}

std::string JZHttpUploadHandler::missingRangesJson(const Session& s, int64_t total)
{
    std::string out = "[";
    int64_t cur = 0;
    bool first = true;
    for (const auto& p : s.ranges)
    {
        if (p.first > cur)
        {
            if (!first) out += ",";
            out += "[" + std::to_string(cur) + "," + std::to_string(p.first) + "]";
            first = false;
        }
        cur = std::max(cur, p.second);
    }
    if (cur < total)
    {
        if (!first) out += ",";
        out += "[" + std::to_string(cur) + "," + std::to_string(total) + "]";
    }
    out += "]";
    return out;
}

uint32_t JZHttpUploadHandler::crc32(const void* data, int len)
{
    static uint32_t table[256];
    static bool inited = false;
    if (!inited)
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        inited = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (int i = 0; i < len; i++)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

std::string JZHttpUploadHandler::toHex(const JZByteArray& b)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (int i = 0; i < b.size(); i++)
    {
        uint8_t c = static_cast<uint8_t>(b[i]);
        s += d[c >> 4];
        s += d[c & 0xF];
    }
    return s;
}

void JZHttpUploadHandler::sendJson(JZHttpServerClient* client, const JZHttpRequest& req,
                                   int code, const std::string& json)
{
    JZHttpRespone resp;
    resp.setStatusCode(code);
    resp.setJson(json);
    resp.applyContentEncoding(req.header("Accept-Encoding"));
    client->response(resp, req);
}

std::string JZHttpUploadHandler::jsonEscape(const std::string& s)
{
    std::string out;
    for (char c : s)
    {
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    static const char* h = "0123456789abcdef";
                    out += "\\u00";
                    out += h[(c >> 4) & 0xF];
                    out += h[c & 0xF];
                }
                else out += c;
        }
    }
    return out;
}
