#include "JZSendFromDirectory.h"

#include <algorithm>
#include "JZDir.h"
#include "JZFile.h"

JZSendFromDirectory::JZSendFromDirectory(const std::string& rootDir)
{
    m_absRoot = JZFileInfo::weaklyCanonicalPath(JZDir(rootDir).absolutePath());
}

// 核心：路径安全检查（绝对无法绕过）
std::string JZSendFromDirectory::localPath(const std::string& userPath)
{
    string web_path = userPath;
    if (str_endsWith(userPath, "/"))
        web_path += "index.html";

    JZDir dir(m_absRoot);
    string path = dir.absoluteFilePath(web_path);
    return JZFileInfo::weaklyCanonicalPath(path);
}

void JZSendFromDirectory::process(const JZHttpRequest& req, JZHttpRespone& resp)
{
    std::string local_path = localPath(req.path());
    if (!str_startsWith(local_path, m_absRoot))
    {
        resp.setStatusCode(403);
        resp.setBody("403 Forbidden: Cannot access outside root directory");
        return;
    }
    // 发送文件
    if (sendFile(local_path, resp))
        return;

    // 404
    resp.setStatusCode(404);
    resp.setBody("404 Not Found");
}

void JZSendFromDirectory::operator()(const JZHttpRequest& req, JZHttpRespone& resp)
{
    process(req, resp);
}

bool JZSendFromDirectory::sendFile(const std::string& filePath, JZHttpRespone& resp)
{
    JZFile file;
    if (!file.open(filePath, "rb"))
        return false;
    
    resp.setStatusCode(200);
    resp.setBody(file.readAll());
    resp.setHeader("Content-Type", getContentType(filePath));
    file.close();
    return true;
}

std::string JZSendFromDirectory::getContentType(const std::string& path)
{
    size_t pos = path.find_last_of('.');
    if (pos == std::string::npos)
        return "application/octet-stream";

    std::string ext = path.substr(pos);
    if (ext == ".html")
        return "text/html; charset=utf-8";
    if (ext == ".css")
        return "text/css; charset=utf-8";
    if (ext == ".js")
        return "application/javascript; charset=utf-8";
    if (ext == ".png")
        return "image/png";
    if (ext == ".jpg")
        return "image/jpeg";
    if (ext == ".jpeg")
        return "image/jpeg";
    if (ext == ".ico")
        return "image/x-icon";
    if (ext == ".gif")
        return "image/gif";
    if (ext == ".svg")
        return "image/svg+xml";

    return "application/octet-stream";
}