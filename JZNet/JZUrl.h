#ifndef JZ_URL_H_
#define JZ_URL_H_

#include <string>
#include <map>
#include "JZGlobal.h"

class JZUrlQuery
{
public:
    JZUrlQuery() = default;

    void clear();

    void addQueryItem(const std::string& key, const std::string& value);
    void removeQueryItem(const std::string& key);

    std::string toString() const;
    void fromString(const std::string& queryStr);

private:
    std::map<std::string, std::string> m_queryItems;
};

//JZUrl
class JZUrl
{
public:
    JZUrl();
    explicit JZUrl(std::string url);
    
    std::string url() const;
    void setUrl(const std::string &url);
    void clear();

    const std::string& scheme() const;
    const std::string& host() const;
    uint16_t port() const;

    const JZUrlQuery &query();
    void setQuery(const JZUrlQuery& query);

private:
    void parse_url(std::string original_url);
    void set_default_port();
    bool isDefaultPort() const;

    std::string m_scheme;
    std::string m_host;
    std::string m_path;
    uint16_t m_port;
    JZUrlQuery m_query;
};


#endif  // !JZ_URL_H_
