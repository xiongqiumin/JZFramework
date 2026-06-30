#ifndef JZ_SEND_FROM_DIRECTORY_H
#define JZ_SEND_FROM_DIRECTORY_H

#include <string>

#include "JZHttpHandler.h"

class JZHttpStaticFiles : public JZHttpHandler
{
public:
    explicit JZHttpStaticFiles(const std::string& rootDir);
    void handle(JZHttpServerClient* client, const JZHttpRequest& req) override;

private:
    std::string localPath(const std::string& userPath);
    bool sendFile(const std::string& filePath, JZHttpRespone& resp);
    std::string getContentType(const std::string& path);
    std::string m_absRoot;
    bool m_isResource = false;
};

#endif
