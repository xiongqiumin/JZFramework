#include "JZTxClient.h"
#include "JZTx.h"
#include "JZNetworkAccessManager.h"
#include "JZStd.h"
#include "JZDebug.h"

bool JZTxClient::syncSend(const JZByteArray& buffer)
{
    // 4. 如果有数据则发送，否则跳过
    JZNetworkReply* reply = m_mgr->post(
        JZNetworkRequest("http://192.168.111.1:5000/"),
        buffer
    );
    JZEventLoop loop;
    connect(&reply->sigFinished, [reply, &loop]() {
        reply->deleteLater();
        loop.quit();
    });
    loop.exec();

    if (reply->error() != JZNetworkReply::None)
    {
        jzWarning() << "JZTxClient send failed" << reply->errorString();
        return false;
    }
    return true;
}

void JZTxClient::run()
{
    JZNetworkAccessManager mgr;
    m_mgr = &mgr;

    std::map<std::string, int> str_map;   // 跨批次持久化
    int str_id = 0;

    auto inst = JZTxManager::instance();
    //std::string echo = { "name": +inst->name
    //syncSend(echo);

    while (true) {
        // 1. 从全局管理器中取出日志列表（加锁）
        inst->m_mutex.lock();
        std::vector<JZTxTraceLog> logs = std::move(inst->m_logList);
        inst->m_logList.clear();
        inst->m_mutex.unlock();

        // 2. 如果没有日志，等待后继续
        if (logs.empty()) {
            msleep(1000);
            continue;
        }

        // 3. 构建 JSONL（每行一个 JSON 数组）
        JZByteArray buffer;

        for (const auto& log : logs) {
            // 3.1 根据类型决定是否处理
            if (log.type == JZTxTraceLog::TraceNone) {
                continue;   // 跳过 TraceNone（不发送）
            }

            // 3.2 准备字符串池化（仅当 type == TraceIn 时需要 tag）
            bool isTraceIn = (log.type == JZTxTraceLog::TraceIn);
            std::string tagStr;
            int tagId = -1;
            bool isNewTag = false;

            if (isTraceIn) {
                tagStr.assign(log.tag.str, log.tag.len);
                auto it = str_map.find(tagStr);
                if (it == str_map.end()) {
                    tagId = str_id++;
                    str_map.emplace(tagStr, tagId);
                    isNewTag = true;
                }
                else {
                    tagId = it->second;
                }
            }

            // 3.3 拼接单行数组（紧凑）
            std::string line;
            line.reserve(128);
            line.push_back('[');

            // type (始终输出)
            line.append(std::to_string(static_cast<int>(log.type)));
            line.push_back(',');

            // timestamp (始终输出)
            line.append(std::to_string(log.timestamp));
            line.push_back(',');

            // tid (始终输出)
            line.append(std::to_string(log.tid));

            // 如果是 TraceIn，追加 tag 和 stackUsed
            if (isTraceIn) {
                line.push_back(',');

                // tag：新字符串输出对象，已有字符串仅输出数字 id
                if (isNewTag) {
                    line.append("{\"id\":");
                    line.append(std::to_string(tagId));
                    line.append(",\"value\":\"");
                    line.append(str_escape(tagStr));
                    line.push_back('"');
                    line.push_back('}');
                }
                else {
                    line.append(std::to_string(tagId));   // 直接输出数字 id
                }

                // stackUsed
                line.push_back(',');
                line.append(std::to_string(log.stackUsed));
            }

            // 其他类型（TraceOut）不追加额外字段

            line.push_back(']');
            line.push_back('\n');   // JSONL 行分隔

            buffer.append(line.c_str(), line.size());
        }
        msleep(1000);
    }

    //std::string echo = { "name": +inst->name
    //syncSend(echo);
}