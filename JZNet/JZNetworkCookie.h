#ifndef JZ_NETWORK_COOKIE_H_
#define JZ_NETWORK_COOKIE_H_
#include <string>
#include <vector>
#include <cstdint>
#include "JZGlobal.h"

class JZUrl;

// 客户端保存的一条 Cookie（来自响应的 Set-Cookie）。
struct JZFRAMEWORK_API JZNetworkCookie
{
    std::string name;
    std::string value;
    std::string domain;          // 小写, 不含前导 '.'
    std::string path = "/";
    bool hostOnly = true;        // true: 只发给 domain 精确相等的 host（无 Domain 属性）
    bool secure = false;         // true: 只对 https 发送
    bool httpOnly = false;       // 仅记录（本框架可直接读, 不做 JS 沙箱限制）
    int64_t expiresAt = 0;       // 绝对毫秒时间戳; 0 = 会话 cookie(进程内一直有效)

    bool isExpired(int64_t nowMs) const;
    // RFC 6265 的匹配规则(域 / 路径前缀 / Secure)
    bool matches(const std::string& scheme, const std::string& host, const std::string& path) const;
};

/*
 * 客户端 Cookie 自动存取（浏览器 cookie jar 的最小版）：
 *   - 收: 响应里的 Set-Cookie 全部存下来（同名同域同路径覆盖；Max-Age/Expires 到期的删掉）；
 *   - 发: 生成某请求该带的 "Cookie" 头值（按域/路径/Secure 过滤, 顺带清理过期项）。
 *
 * 只做「存取」，不做登录态校验 —— sid 对应的用户是谁由业务自己查表。
 */
class JZFRAMEWORK_API JZNetworkCookieJar
{
public:
    void clear();
    int count() const { return (int)m_cookies.size(); }
    const std::vector<JZNetworkCookie>& cookies() const { return m_cookies; }

    // 解析响应里的所有 Set-Cookie 头并保存。nowMs 用墙钟毫秒（不是单调时钟）。
    void saveFromResponse(const JZUrl& url, const std::vector<std::string>& setCookieHeaders, int64_t nowMs);

    // 生成该请求的 "Cookie" 头值（"a=1; b=2"）；无匹配返回空串。
    std::string headerFor(const JZUrl& url, int64_t nowMs);

private:
    void purgeExpired(int64_t nowMs);
    std::vector<JZNetworkCookie> m_cookies;
};

#endif // !JZ_NETWORK_COOKIE_H_
