#ifndef JZ_SEND_FROM_DIRECTORY_H
#define JZ_SEND_FROM_DIRECTORY_H
#include <map>
#include <string>

#include "JZFile.h"
#include "JZHttpHandler.h"

class JZFRAMEWORK_API JZHttpStaticFiles : public JZHttpHandler
{
public:
    explicit JZHttpStaticFiles(const std::string& rootDir);
    void handle(JZHttpServerClient* client, const JZHttpRequest& req) override;
    void onNetEvent(JZHttpServerClient* client, JZNetResEvent *event) override;

    // 每次流式发送的最大块大小（字节）。默认 64K，可通过 setChunkSize 调整。
    void setChunkSize(int size) { if (size > 0) m_chunkSize = size; m_streamBuffer.resize(size);  }
    int chunkSize() const { return m_chunkSize; }

    // 浏览器缓存时长(秒)。默认 3600。**改了页面就希望立刻生效**的宿主(管理台)应设 0:
    // 这时发 max-age=0 + ETag/Last-Modified, 浏览器每次回源校验, 没变就 304(不重传)。
    // ⚠ 不改这个的话, 换了 app.js 而浏览器还跑着缓存里的旧版, 页面会出现
    //   "HTML 是新的/脚本是旧的" 这种最难查的空白页。
    void setCacheMaxAge(int sec) { m_cacheMaxAge = sec < 0 ? 0 : sec; }
    int cacheMaxAge() const { return m_cacheMaxAge; }

    // 预压缩静态文件：若磁盘存在 <file>.gz / <file>.br 且客户端 Accept-Encoding 接受，
    // 直接发送预压缩字节（零运行时压缩开销），Content-Type 按原始文件名决定。
    // 例：请求 app.js 且客户端接受 gzip，且存在 app.js.gz → 直接发 app.js.gz（Content-Encoding: gzip）。
    // 不存在预压缩变体时，按原文件流式发送（不压缩）。

private:
    // 按连接保存的文件流状态（一个 handler 被所有连接共享，用 client 指针区分，并发安全）
    struct StreamState
    {
        JZFile file;
        int remainSize;
        int pendingSize;
        int sigDestroyedId = -1;
    };

    bool openStreamFile(JZHttpServerClient* client, const std::string& path);
    void writeStreamFile(JZHttpServerClient* client);
    void closeStreamFile(JZHttpServerClient* client);

    std::string localPath(const std::string& userPath);
    std::string getContentType(const std::string& path);
    
    std::string m_absRoot;
    int m_chunkSize = 64 * 1024;
    int m_cacheMaxAge = 3600;  // 缓存时长（秒），默认 1 小时
    std::map<JZHttpServerClient*, StreamState> m_streams;
    JZByteArray m_streamBuffer;
};

#endif
