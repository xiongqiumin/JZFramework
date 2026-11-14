#ifndef JZ_HTTP_RESPONE_H_
#define JZ_HTTP_RESPONE_H_
#include <string>
#include <map>
#include <vector>
#include "JZByteArray.h"
#include "JZCompress.h"
#include "JZHttpCookie.h"

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
    // 设置响应头。key 为 Set-Cookie 时**追加**一条而不是覆盖（Set-Cookie 允许多条），
    // 等价于 setCookie(JZHttpCookie(name, value))；header("Set-Cookie") 读回最后一条。
    void setHeader(const std::string& key, const std::string& value);
    // 移除响应头。key 为 Set-Cookie 时清空本响应已累积的全部 cookie。
    void removeHeader(const std::string& key);
    const std::map<std::string, std::string>& headers() const;

    // 追加一条 Set-Cookie（同一个响应可以下发多条，例如同时设置会话与偏好）。
    // 用 cookie() 区分用户时，通常在这里下发 sid；请求侧用 JZHttpRequest::cookie("sid") 读回。
    void setCookie(const JZHttpCookie& cookie);
    void setCookie(const std::string& name, const std::string& value);
    void clearCookies();
    // 已累积的 Set-Cookie 头值（按加入顺序，不含 "Set-Cookie: " 前缀）
    const std::vector<std::string>& cookieLines() const;

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
    std::vector<std::string> m_cookieLines;   // 多条 Set-Cookie 的头值, 序列化时逐条输出
    JZByteArray m_body;
};

#endif // !JZ_HTTP_RESPONE_H_
