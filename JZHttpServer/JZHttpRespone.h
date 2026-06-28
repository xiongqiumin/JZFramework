#ifndef JZ_HTTP_RESPONE_H_
#define JZ_HTTP_RESPONE_H_

#include <string>
#include <map>
#include "JZByteArray.h"

class JZHttpRespone
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

    static std::string getStatusText(int code);

private:
    int m_statusCode;
    std::string m_statusText;
    std::map<std::string, std::string> m_headers;
    JZByteArray m_body;
};

#endif // !JZ_HTTP_RESPONE_H_
