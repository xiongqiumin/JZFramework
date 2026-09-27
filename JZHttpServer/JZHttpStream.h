#ifndef JZ_HTTP_STREAM_H_
#define JZ_HTTP_STREAM_H_
#include <string>
#include <map>
#include "JZObject.h"
#include "JZCompress.h"

class JZHttpServerClient;
class JZByteArray;

/*
 * JZHttpStream: 统一的 HTTP 流式响应（分块传输）。
 *
 * SSE 只是"流式 + text/event-stream 帧格式"，与通用流式在传输层是同一回事，
 * 因此这里只保留一个类：SSE 作为配置项（setSSE），send()/comment() 提供 SSE 帧封装。
 *
 * 能力：
 *   - 分块写入、保持长连接（不输出 Content-Length）。
 *   - SSE 模式：open() 写 text/event-stream 头；send()/comment() 叠 SSE 帧。
 *   - 流式压缩：setContentEncoding() 后，每块数据经 JZCompressStream 实时压缩再发送
 *     （gzip/deflate），Content-Encoding 自动设置。
 *
 * 生命周期：构造时把 client 设为父对象。客户端断开时 client 析构会先删本流，
 *           handler 无需手动 delete / connect sigDestroyed。推送时机由业务驱动。
 */
class JZFRAMEWORK_API JZHttpStream : public JZObject
{
public:
    explicit JZHttpStream(JZHttpServerClient* client);
    ~JZHttpStream();

    // ---- 配置（open 前调用） ----
    void setStatusCode(int code);
    void setContentType(const std::string& type);
    void setHeader(const std::string& key, const std::string& value);
    void setSSE(bool on = true);                        // SSE 模式（open 时写 text/event-stream 头）
    void setContentEncoding(JZCompressAlgorithm algo);  // 流式压缩（gzip/deflate）
    void setCompressLevel(int level);                   // 压缩等级，-1=默认

    // 开启响应：发送已配置响应头（含 Content-Encoding），不输出 Content-Length，保持长连接。
    // 启用压缩时同时初始化压缩流。
    void open();

    // 写入一块原始数据；is_finish=true 结束响应。启用压缩时数据先被实时压缩。
    void write(const JZByteArray& data, bool is_finish = false);
    void write(const char* data, int size, bool is_finish = false);

    // SSE 事件：仅当 setSSE(true) 时语义完整（内部按 SSE 帧封装后写入）。
    void send(const std::string& data,
              const std::string& event = std::string(),
              const std::string& id = std::string(),
              int retry = -1);
    void comment(const std::string& text);

    void close();
    bool isOpen() const { return m_open; }
    bool isClosed() const { return m_closed || !m_client; }
    JZHttpServerClient* client() const { return m_client; }

private:
    static const char* httpEncodingName(JZCompressAlgorithm algo);

    JZHttpServerClient* m_client = nullptr;
    int m_statusCode = 200;
    std::map<std::string, std::string> m_headers;
    bool m_sse = false;
    bool m_compressEnabled = false;
    JZCompressAlgorithm m_algo = JZCompressAlgorithm::GZip;
    int m_level = -1;
    JZCompressStream* m_compress = nullptr;
    bool m_open = false;
    bool m_closed = false;
};

#endif // JZ_HTTP_STREAM_H_
