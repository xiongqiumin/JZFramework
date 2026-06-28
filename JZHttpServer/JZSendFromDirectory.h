#ifndef JZ_SEND_FROM_DIRECTORY_H
#define JZ_SEND_FROM_DIRECTORY_H

#include <string>

#include "JZHttpRequest.h"
#include "JZHttpRespone.h"

class JZSendFromDirectory
{
public:
    explicit JZSendFromDirectory(const std::string& rootDir);
    void operator()(const JZHttpRequest& req, JZHttpRespone& resp);

private:
    std::string localPath(const std::string& userPath);
    bool sendFile(const std::string& filePath, JZHttpRespone& resp);
    std::string getContentType(const std::string& path);

private:
    void process(const JZHttpRequest& req, JZHttpRespone& resp);
    std::string m_absRoot;
};

#endif