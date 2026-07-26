#include "JZHttpStaticFiles.h"

#include <algorithm>
#include <cstdio>
#include "JZDir.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZHttpServer.h"

JZHttpStaticFiles::JZHttpStaticFiles(const std::string& rootDir)
{
    // 以 ':' 开头的 rootDir 表示资源路径（如 ":/xxx"），否则为文件系统目录。
    // 是否资源路径直接由 rootDir 文件名判断，无需额外标志位。
    if (str_startsWith(m_absRoot, ":/"))
        m_absRoot = rootDir;
    else
        m_absRoot = JZFileInfo::weaklyCanonicalPath(JZDir(rootDir).absolutePath());

    setChunkSize(m_chunkSize);
}

std::string JZHttpStaticFiles::localPath(const std::string& userPath)
{
    std::string web_path = userPath;
    if (str_endsWith(userPath, "/"))
        web_path += "index.html";

    if (str_startsWith(m_absRoot, ":/"))
    {
        // 资源路径：去掉 : 前缀 → canonicalize → 加回 :
        std::string raw = m_absRoot + web_path;  // web_path 已带 /
        return ":" + JZFileInfo::weaklyCanonicalPath(raw.substr(1));
    }

    // 始终在根目录下解析：web_path 以 '/' 开头，直接拼到 m_absRoot 后规范化，
    // 避免 JZDir::absoluteFilePath 把以 '/' 开头的请求路径当作绝对路径而忽略根目录（导致 403）
    std::string path = m_absRoot + web_path;
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

    // 文件不存在直接 404
    if (!openStreamFile(client, local_path))
    {
        resp.setStatusCode(404);
        resp.setBody("404 Not Found");
        client->response(resp.toByteArray());
        return;
    }

    // 文件存在：先发响应头 + 第一块，后续块由 onNetEvent 的写完成事件驱动
    resp.setStatusCode(200);
    resp.setHeader("Content-Type", getContentType(local_path));

    StreamState& st = m_streams[client];
    JZByteArray head = resp.toHeadByteArray(st.remainSize);
    client->write(head, false);
    st.pendingSize += head.size();    
    writeStreamFile(client);
}

void JZHttpStaticFiles::onNetEvent(JZHttpServerClient* client, JZNetResEvent *event)
{
    auto it = m_streams.find(client);
    if (it == m_streams.end())
        return;

    if (event->event == JZ_NET_EV_WRITE)
    {
        JZDataStream s(&event->binData);
        int write_size = 0;
        s >> write_size;

        it->second.pendingSize -= write_size;
        if (it->second.pendingSize == 0)
            writeStreamFile(client);
    }
}

bool JZHttpStaticFiles::openStreamFile(JZHttpServerClient* client, const std::string& path)
{
    StreamState& st = m_streams[client];   // 默认构造（file 关闭、size 0），避免 JZFile 不可拷贝/移动
    if (!st.file.open(path, "rb"))
    {
        m_streams.erase(client);
        return false;
    }
    client->setHandle(this);
    JZObjectPointer self = guardPointer();
    connect(&client->sigDestroyed, this, [this, client](JZObject*){
        m_streams.erase(client);
    });

    st.remainSize = st.file.size();
    return true;
}


void JZHttpStaticFiles::writeStreamFile(JZHttpServerClient* client)
{
    auto it = m_streams.find(client);
    if (it == m_streams.end())
        return;

    int size = it->second.file.read(m_streamBuffer.data(), m_streamBuffer.size());
    it->second.pendingSize += size;
    it->second.remainSize -= size;
    bool is_last = (it->second.remainSize == 0);
    client->write(m_streamBuffer.data(), m_streamBuffer.size(), is_last);
    if (is_last)
        closeStreamFile(client);
}

void JZHttpStaticFiles::closeStreamFile(JZHttpServerClient* client)
{
    m_streams.erase(client);
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
