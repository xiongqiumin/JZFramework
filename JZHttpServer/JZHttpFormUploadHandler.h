#ifndef JZ_HTTP_FORM_UPLOAD_HANDLER_H_
#define JZ_HTTP_FORM_UPLOAD_HANDLER_H_

#include "JZHttpHandler.h"
#include <string>

/*
 * JZHttpFormUploadHandler — 普通 multipart/form-data (RFC 7578) 单请求上传。
 *
 * 底层直接复用 mongoose 自带的解析器 mg_http_next_multipart(零拷贝, 不需要自己写解析)。
 *
 * 挂载示例:
 *   JZHttpFormUploadHandler formUpload;
 *   formUpload.setUploadDir("./uploads");
 *   server.addRoute("/upload/form", &formUpload);   // 精确路由, 不会被 /upload/<key> 抢走
 *
 * 用法:
 *   POST /upload/form
 *     Content-Type: multipart/form-data; boundary=----xxx
 *     Body: 标准 multipart, 每个带 filename 的 part 视为一个文件, 其余视为表单字段
 *   成功 200:
 *     {"status":"ok","files":[{"name","size","md5","path"}...],"fields":{...}}
 *   失败: {"status":"error","msg":"..."}
 *
 * 说明:
 *   - 单请求一次性上传, 不支持断点续传(要续传请用 JZHttpUploadHandler)。
 *   - 文件名经 sanitize(去目录成分 / ".." / 非法字符), 且同名自动加 -N 后缀, 防覆盖与路径穿越。
 *   - 完成时按 m_writeMd5 写出 "<文件名>.md5" 校验和侧车。
 */
class JZHttpFormUploadHandler : public JZHttpHandler
{
public:
    JZHttpFormUploadHandler();
    ~JZHttpFormUploadHandler();

    void setUploadDir(const std::string& dir);
    void setWriteMd5Sidecar(bool on);

    void handle(JZHttpServerClient* client, const JZHttpRequest& req) override;

private:
    std::string m_uploadDir = "uploads";
    bool m_writeMd5 = true;
};

#endif // !JZ_HTTP_FORM_UPLOAD_HANDLER_H_
