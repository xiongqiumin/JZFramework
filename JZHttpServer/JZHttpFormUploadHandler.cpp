#include "JZHttpFormUploadHandler.h"

#include "JZHttpServer.h"
#include "JZHttpRespone.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZDir.h"
#include "JZCrypto.h"
#include "JZByteArray.h"
#include "mongoose.h"

#include <string>
#include <vector>
#include <cctype>

namespace {

// 去掉目录成分 / ".." / 非法文件名字符, 并避免纯点文件名 -> 防止路径穿越与覆盖系统文件
std::string sanitizeFileName(const std::string& name)
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
        if ((unsigned char)c < 0x20) continue;   // 控制字符
        out += c;
    }
    size_t i = 0;
    while (i < out.size() && out[i] == '.') i++;  // 去掉前导点(避免 . / .. 残留)
    out = out.substr(i);
    if (out.empty()) out = "upload";
    return out;
}

// 同名自动加 -N 后缀, 避免覆盖已有文件
std::string uniquePath(const std::string& path)
{
    if (!JZFile::exists(path)) return path;
    size_t dot = path.find_last_of('.');
    size_t sep = path.find_last_of("/\\");
    std::string base = path, ext;
    if (dot != std::string::npos && (sep == std::string::npos || dot > sep))
    {
        base = path.substr(0, dot);
        ext  = path.substr(dot);
    }
    for (int i = 1; i < 100000; i++)
    {
        std::string cand = base + "-" + std::to_string(i) + ext;
        if (!JZFile::exists(cand)) return cand;
    }
    return base + "-x" + ext;
}

std::string toHex(const JZByteArray& b)
{
    static const char* h = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (int i = 0; i < b.size(); i++)
    {
        unsigned char c = (unsigned char)b[i];
        s += h[c >> 4];
        s += h[c & 0xF];
    }
    return s;
}

std::string jsonEscape(const std::string& s)
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
                if ((unsigned char)c < 0x20)
                {
                    static const char* hx = "0123456789abcdef";
                    out += "\\u00";
                    out += hx[(c >> 4) & 0xF];
                    out += hx[c & 0xF];
                }
                else out += c;
        }
    }
    return out;
}

void sendJson(JZHttpServerClient* client, const JZHttpRequest& req, int code, const std::string& json)
{
    JZHttpRespone resp;
    resp.setStatusCode(code);
    resp.setJson(json);
    resp.applyContentEncoding(req.header("Accept-Encoding"));
    client->response(resp, req);
}

} // namespace

JZHttpFormUploadHandler::JZHttpFormUploadHandler() = default;
JZHttpFormUploadHandler::~JZHttpFormUploadHandler() = default;

void JZHttpFormUploadHandler::setUploadDir(const std::string& dir)
{
    m_uploadDir = dir;
}

void JZHttpFormUploadHandler::setWriteMd5Sidecar(bool on)
{
    m_writeMd5 = on;
}

void JZHttpFormUploadHandler::handle(JZHttpServerClient* client, const JZHttpRequest& req)
{
    if (req.method() != JZHttpRequest::POST)
    {
        sendJson(client, req, 405, "{\"status\":\"error\",\"msg\":\"method not allowed, use POST\"}");
        return;
    }

    const std::string& ct = req.contentType();
    if (ct.find("multipart/form-data") == std::string::npos)
    {
        sendJson(client, req, 400, "{\"status\":\"error\",\"msg\":\"expect Content-Type: multipart/form-data\"}");
        return;
    }

    const JZByteArray& body = req.body();
    if (body.size() == 0)
    {
        sendJson(client, req, 400, "{\"status\":\"error\",\"msg\":\"empty body\"}");
        return;
    }

    JZDir().mkpath(m_uploadDir);

    // 复用到来的原始 body 缓冲(零拷贝), mg_http_part 的 body 指向其中, 处理期间有效
    struct mg_str mb = { const_cast<char*>(body.constData()), (size_t)body.size() };
    struct mg_http_part part;
    size_t ofs = 0;

    std::string filesJson  = "[";
    std::string fieldsJson = "{";
    bool firstFile = true, firstField = true;

    while ((ofs = mg_http_next_multipart(mb, ofs, &part)) != 0)
    {
        std::string name(part.name.buf, part.name.len);
        std::string filename(part.filename.buf, part.filename.len);
        JZByteArray data(part.body.buf, (int)part.body.len);

        if (!filename.empty())
        {
            // 文件 part
            std::string safe = sanitizeFileName(filename);
            if (safe.empty()) safe = "upload.bin";
            std::string dst = uniquePath(m_uploadDir + "/" + safe);

            JZFile f;
            if (!f.open(dst, "wb"))
            {
                sendJson(client, req, 500, "{\"status\":\"error\",\"msg\":\"cannot write file\"}");
                return;
            }
            f.write(data);
            f.close();

            int64_t sz = JZFileInfo(dst).size();
            std::string hex = toHex(JZCrypto::md5(data));
            if (m_writeMd5)
            {
                JZFile mf;
                if (mf.open(dst + ".md5", "wb")) mf.write(hex);
            }

            if (!firstFile) filesJson += ",";
            firstFile = false;
            filesJson += "{\"name\":\"" + jsonEscape(safe) +
                         "\",\"size\":" + std::to_string(sz) +
                         ",\"md5\":\"" + hex +
                         "\",\"path\":\"" + jsonEscape(dst) + "\"}";
        }
        else if (!name.empty())
        {
            // 普通表单字段
            if (!firstField) fieldsJson += ",";
            firstField = false;
            std::string val((const char*)data.constData(), data.size());
            fieldsJson += "\"" + jsonEscape(name) + "\":\"" + jsonEscape(val) + "\"";
        }
    }

    filesJson  += "]";
    fieldsJson += "}";
    sendJson(client, req, 200,
             "{\"status\":\"ok\",\"files\":" + filesJson + ",\"fields\":" + fieldsJson + "}");
}
