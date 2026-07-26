#ifndef JZ_SEND_FROM_DIRECTORY_H
#define JZ_SEND_FROM_DIRECTORY_H

#include <map>
#include <string>

#include "JZFile.h"
#include "JZHttpHandler.h"

class JZHttpStaticFiles : public JZHttpHandler
{
public:
    explicit JZHttpStaticFiles(const std::string& rootDir);
    void handle(JZHttpServerClient* client, const JZHttpRequest& req) override;
    void onNetEvent(JZHttpServerClient* client, JZNetResEvent *event) override;

    // 每次流式发送的最大块大小（字节）。默认 64K，可通过 setChunkSize 调整。
    void setChunkSize(int size) { if (size > 0) m_chunkSize = size; m_streamBuffer.resize(size);  }
    int chunkSize() const { return m_chunkSize; }

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
