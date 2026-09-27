#ifndef JZ_HTTP_RESPONE_H_
#define JZ_HTTP_RESPONE_H_
#include <string>
#include <map>
#include "JZByteArray.h"
#include "JZCompress.h"

class JZFRAMEWORK_API JZHttpRespone
{
public:
    JZHttpRespone();
    ~JZHttpRespone();

    int statusCode() const;
    void setStatusCode(int code);

    const std::string& statusText() const;
    void setStatusText(const std::string& text);

    const std::string& header(const std::string& key) const;
    void setHeader(const std::string& key, const std::string& value);
    void removeHeader(const std::string& key);
    const std::map<std::string, std::string>& headers() const;

    const JZByteArray& body() const;
    void setBody(const JZByteArray& body);
    void setBody(const std::string& body);

    const std::string& contentType() const;
    void setContentType(const std::string& type);

    void setText(const std::string& text);
    void setHtml(const std::string& html);
    void setJson(const std::string& json);
    void setFile(const JZByteArray& data, const std::string& filename);

    JZByteArray toByteArray() const;
    // 仅序列化响应头，Content-Length 使用传入的值（用于流式分块发送 body）
    JZByteArray toHeadByteArray(int contentLength) const;
    // 与上一函数相同，includeContentLength=false 时不输出 Content-Length 行
    // （用于 SSE 等长连接流式响应，长度未知）
    JZByteArray toHeadByteArray(int contentLength, bool includeContentLength) const;

    // 根据请求 Accept-Encoding 自动选择本库支持的压缩算法(gzip/deflate)压缩正文，
    // 设置 Content-Encoding / Vary 头；成功返回 true。仅对整个响应(正文已就绪)有效，
    // 流式响应请使用 JZHttpStream(已内置流式压缩) / JZCompressStream。
    bool applyContentEncoding(const std::string& acceptEncoding);
    const std::string& contentEncoding() const;

    static std::string getStatusText(int code);

    // 解析 Accept-Encoding，选出本库支持的内容编码。成功返回 true，
    // outAlgo 为对应 JZCompressAlgorithm，outName 为 Content-Encoding 取值("gzip"/"deflate")。
    static bool selectContentEncoding(const std::string& acceptEncoding,
                                     JZCompressAlgorithm& outAlgo,
                                     std::string& outName);

private:
    int m_statusCode;
    std::string m_statusText;
    std::map<std::string, std::string> m_headers;
    JZByteArray m_body;
};

#endif // !JZ_HTTP_RESPONE_H_
