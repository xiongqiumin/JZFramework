#include "JZHttpStaticFiles.h"

#include <algorithm>
#include "JZDir.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZHttpServer.h"

JZHttpStaticFiles::JZHttpStaticFiles(const std::string& rootDir)
{
    if (!rootDir.empty() && rootDir[0] == ':')
    {
        m_isResource = true;
        m_absRoot = rootDir;
    }
    else
    {
        m_isResource = false;
        m_absRoot = JZFileInfo::weaklyCanonicalPath(JZDir(rootDir).absolutePath());
    }
}

std::string JZHttpStaticFiles::localPath(const std::string& userPath)
{
    std::string web_path = userPath;
    if (str_endsWith(userPath, "/"))
        web_path += "index.html";

    if (m_isResource)
    {
        // 去掉 : 前缀 → canonicalize → 加回 :
        std::string raw = m_absRoot + web_path;  // web_path 已带 /
        return ":" + JZFileInfo::weaklyCanonicalPath(raw.substr(1));
    }

    JZDir dir(m_absRoot);
    std::string path = dir.absoluteFilePath(web_path);
    return JZFileInfo::weaklyCanonicalPath(path);
}

void JZHttpStaticFiles::handle(JZHttpServerClient* client, const JZHttpRequest& req)
{
    JZHttpRespone resp;
    std::string local_path = localPath(req.path());

    if (!str_startsWith(local_path, m_absRoot))
    {
        resp.setStatusCode(403);
        resp.setBody("403 Forbidden");
        client->response(resp.toByteArray());
        return;
    }

    if (sendFile(local_path, resp))
    {
        client->response(resp.toByteArray());
        return;
    }

    resp.setStatusCode(404);
    resp.setBody("404 Not Found");
    client->response(resp.toByteArray());
}

bool JZHttpStaticFiles::sendFile(const std::string& filePath, JZHttpRespone& resp)
{
    JZFile file;
    if (!file.open(filePath, "rb"))
        return false;
    
    resp.setStatusCode(200);
    resp.setHeader("Content-Type", getContentType(filePath));
    resp.setBody(file.readAll());
    file.close();
    return true;
}

std::string JZHttpStaticFiles::getContentType(const std::string& path)
{
    size_t pos = path.find_last_of('.');
    if (pos == std::string::npos)
        return "application/octet-stream";

    std::string ext = path.substr(pos);
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".css")  return "text/css; charset=utf-8";
    if (ext == ".js")   return "application/javascript; charset=utf-8";
    if (ext == ".png")  return "image/png";
    if (ext == ".jpg")  return "image/jpeg";
    if (ext == ".jpeg") return "image/jpeg";
    if (ext == ".ico")  return "image/x-icon";
    if (ext == ".gif")  return "image/gif";
    if (ext == ".svg")  return "image/svg+xml";

    return "application/octet-stream";
}
