#ifndef JZ_HTTP_UPLOAD_HANDLER_H_
#define JZ_HTTP_UPLOAD_HANDLER_H_

#include "JZHttpHandler.h"
#include <string>
#include <map>
#include <vector>
#include <utility>
#include "JZMutex.h"

/*
 * JZHttpUploadHandler — 自定义分块上传协议处理器。
 *
 * 挂载示例:
 *   JZHttpUploadHandler upload;
 *   upload.setUploadDir("./uploads");
 *   server.addRoute("/upload/<key>", &upload);
 *
 * 同一上传会话用路径里的 <key> 标识。通过 query 参数区分阶段,Body 始终为原始字节:
 *   - 初始化  POST /upload/<key>?init          (可选 name=<文件名> size=<总字节> md5=<hex>)
 *   - 分块    POST /upload/<key>?offset=<N>    (Body=该块字节; 可选 crc=<hex> 做分块校验)
 *   - 完成    POST /upload/<key>?complete       (服务端校验大小/缺块/MD5, 落最终文件, 写 .md5 侧车)
 *   - 状态    GET  /upload/<key>                (返回 received/expected/missing 区间, 供断点续传)
 *
 * 校验策略:
 *   1) 分块级(可选): 客户端在 crc 参数带上本块 CRC32(hex), 服务端写入前核对, 不符则 400 拒绝该块。
 *   2) 整文件级: init 提供 md5(hex); complete 时服务端对拼接后的文件算 MD5 比对, 不符则 400。
 *      校验通过后把文件从临时 .part 改名为最终文件, 并按 m_writeMd5 写出 "<文件名>.md5" 校验和侧车。
 */
class JZHttpUploadHandler : public JZHttpHandler
{
public:
    JZHttpUploadHandler();
    ~JZHttpUploadHandler();

    // 上传根目录:分块临时文件(.part)与最终文件均落于此。默认 "uploads"。
    void setUploadDir(const std::string& dir);

    // 完成时是否写出 <文件名>.md5 校验和文件(默认 true)。
    void setWriteMd5Sidecar(bool on);

    void handle(JZHttpServerClient* client, const JZHttpRequest& req) override;

private:
    struct Session
    {
        std::string partPath;                                  // 临时分块文件
        std::string finalName;                                 // 最终文件名(init 的 name 或 key)
        int64_t expectedSize = -1;                             // 期望总大小(-1=未知)
        std::string expectedMd5;                               // 期望 MD5(hex, 空=不校验)
        std::vector<std::pair<int64_t,int64_t>> ranges;        // 已接收区间 [start,end)
    };

    void handleInit(JZHttpServerClient* client, const JZHttpRequest& req, const std::string& key);
    void handleChunk(JZHttpServerClient* client, const JZHttpRequest& req, const std::string& key);
    void handleComplete(JZHttpServerClient* client, const JZHttpRequest& req, const std::string& key);
    void handleStatus(JZHttpServerClient* client, const JZHttpRequest& req, const std::string& key);

    static void mergeRange(Session& s, int64_t start, int64_t end);
    static int64_t receivedBytes(const Session& s);
    static bool rangesCover(const Session& s, int64_t total);
    static std::string missingRangesJson(const Session& s, int64_t total);

    static uint32_t crc32(const void* data, int len);
    static std::string toHex(const JZByteArray& b);

    static void sendJson(JZHttpServerClient* client, const JZHttpRequest& req,
                         int code, const std::string& json);
    static std::string jsonEscape(const std::string& s);

    std::string m_uploadDir = "uploads";
    bool m_writeMd5 = true;
    JZMutex m_mutex;
    std::map<std::string, Session> m_sessions;
};

#endif // !JZ_HTTP_UPLOAD_HANDLER_H_
