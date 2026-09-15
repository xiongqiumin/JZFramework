#include "JZHttpStream.h"
#include "JZHttpServer.h"
#include "JZHttpRespone.h"
#include "JZCompress.h"

const char* JZHttpStream::httpEncodingName(JZCompressAlgorithm algo)
{
    switch (algo)
    {
    case JZCompressAlgorithm::GZip:    return "gzip";
    case JZCompressAlgorithm::Deflate: return "deflate";
    case JZCompressAlgorithm::Zlib:    return "deflate"; // HTTP "deflate" 即 zlib 封装
    }
    return "gzip";
}

JZHttpStream::JZHttpStream(JZHttpServerClient* client)
    : JZObject(client)
{
    m_client = client;
}

JZHttpStream::~JZHttpStream()
{
    if (m_compress)
    {
        delete m_compress;
        m_compress = nullptr;
    }
    m_closed = true;
    m_client = nullptr;
}

void JZHttpStream::setStatusCode(int code) { m_statusCode = code; }
void JZHttpStream::setContentType(const std::string& t) { m_headers["Content-Type"] = t; }
void JZHttpStream::setHeader(const std::string& k, const std::string& v) { m_headers[k] = v; }
void JZHttpStream::setSSE(bool on) { m_sse = on; }
void JZHttpStream::setContentEncoding(JZCompressAlgorithm algo)
{
    m_algo = algo;
    m_compressEnabled = true;
}
void JZHttpStream::setCompressLevel(int level) { m_level = level; }

void JZHttpStream::open()
{
    if (m_open || m_closed || !m_client)
        return;

    JZHttpRespone resp;
    resp.setStatusCode(m_statusCode);
    if (m_sse)
    {
        resp.setContentType("text/event-stream");
        resp.setHeader("Cache-Control", "no-cache");
        resp.setHeader("Connection", "keep-alive");
        resp.setHeader("X-Accel-Buffering", "no");   // 关闭反向代理缓冲（nginx 等）
    }
    for (const auto& kv : m_headers)
        resp.setHeader(kv.first, kv.second);

    if (m_compressEnabled)
    {
        resp.setHeader("Content-Encoding", httpEncodingName(m_algo));
        m_compress = new JZCompressStream(m_algo, m_level);
        m_compress->setBlockSize(0);   // 每次写入立即 flush，最低延迟（适合流式/LLM）
    }

    // 不输出 Content-Length，保持长连接，由后续分块写入维持
    JZByteArray head = resp.toHeadByteArray(0, false);
    m_client->write(head, false);
    m_open = true;
}

void JZHttpStream::write(const char* data, int size, bool is_finish)
{
    if (!m_open || m_closed || !m_client)
        return;

    JZByteArray out;
    if (m_compress)
    {
        if (size > 0)
            out = m_compress->write(JZByteArray(data, size));
        if (is_finish)
        {
            JZByteArray tail = m_compress->finish();
            out.append(tail);
            delete m_compress;
            m_compress = nullptr;
        }
    }
    else if (size > 0)
    {
        out = JZByteArray(data, size);
    }

    if (out.size() > 0)
        m_client->write(out, is_finish);
    else if (is_finish)
        m_client->write(JZByteArray(), true);   // 无体收尾

    if (is_finish)
        m_closed = true;
}

void JZHttpStream::write(const JZByteArray& data, bool is_finish)
{
    write(data.data(), data.size(), is_finish);
}

void JZHttpStream::send(const std::string& data, const std::string& event,
                        const std::string& id, int retry)
{
    if (!m_open || m_closed || !m_client)
        return;

    std::string frame;
    if (!event.empty())
        frame += "event: " + event + "\n";
    if (!id.empty())
        frame += "id: " + id + "\n";
    if (retry > 0)
        frame += "retry: " + std::to_string(retry) + "\n";

    // data 可含多行，每行加 "data: " 前缀；空消息输出 "data:" 占位
    size_t pos = 0;
    bool hasData = false;
    while (pos < data.size())
    {
        size_t nl = data.find('\n', pos);
        std::string line = (nl == std::string::npos) ? data.substr(pos)
                                                      : data.substr(pos, nl - pos);
        frame += "data: " + line + "\n";
        hasData = true;
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
    }
    if (!hasData)
        frame += "data:\n";
    frame += "\n";   // SSE 事件以空行结束

    write(frame.data(), (int)frame.size(), false);
}

void JZHttpStream::comment(const std::string& text)
{
    if (!m_open || m_closed || !m_client)
        return;
    std::string frame = ":" + text + "\n\n";
    write(frame.data(), (int)frame.size(), false);
}

void JZHttpStream::close()
{
    if (m_closed)
        return;
    if (!m_open)
        open();
    write("", 0, true);   // 触发压缩收尾（若有）并结束响应
}
