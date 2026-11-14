#ifndef JZ_HTTP_COOKIE_H_
#define JZ_HTTP_COOKIE_H_
#include <string>
#include <map>
#include "JZGlobal.h"

/*
 * 一条 Cookie。
 *
 * 请求侧(JZHttpRequest::cookies)只用 name/value —— 浏览器发来的 Cookie 头里没有属性。
 * 响应侧(JZHttpRespone::setCookie)除 name/value 外的字段参与 Set-Cookie 序列化，
 * 留空/负数表示不输出该属性。
 */
struct JZFRAMEWORK_API JZHttpCookie
{
    std::string name;
    std::string value;

    std::string path;        // 空 = 不输出；常用 "/"
    std::string domain;      // 空 = 不输出
    std::string expires;     // HTTP-date 原文，如 "Wed, 21 Oct 2026 07:28:00 GMT"；空 = 不输出
    int maxAge = -1;         // <0 = 不输出；0 = 立即过期(删除该 cookie)；单位秒
    bool httpOnly = false;   // 禁止 JS 读取(document.cookie)
    bool secure = false;     // 仅 HTTPS 发送
    std::string sameSite;    // "Lax"/"Strict"/"None"（大小写原样输出）；空 = 不输出

    JZHttpCookie() {}
    JZHttpCookie(const std::string& cookieName, const std::string& cookieValue);

    // 序列化为 Set-Cookie 的头值: "name=value; Path=/; Max-Age=3600; HttpOnly; SameSite=Lax"
    std::string toHeaderValue() const;

    // 解析请求里的 Cookie 头("a=1; b=2")，结果追加进 out(同名后者覆盖)。
    // 值两端的双引号会被去掉，其余原样保留(不做 URL 解码)。
    static void parseRequestHeader(const std::string& header,
                                   std::map<std::string, std::string>& out);
};

#endif // !JZ_HTTP_COOKIE_H_
