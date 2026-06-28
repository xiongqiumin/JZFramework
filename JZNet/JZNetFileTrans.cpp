#include <assert.h>
#include "JZDir.h"
#include "JZFileInfo.h"
#include "JZNetFileTrans.h"
#include "JZEventLoop.h"
#include "JZPlatform.h"
#include "JZLog.h"
#include "JZCrypto.h"
#include "JZStd.h"

JZNetPackFileOperatorReq::JZNetPackFileOperatorReq()
{
    method = Pull;
    taskId = -1;
}

JZNetPackFileOperatorReq::~JZNetPackFileOperatorReq()
{
}

int JZNetPackFileOperatorReq::packetType() const
{
    return NetPack_fileTransReq;
}

void JZNetPackFileOperatorReq::saveToStream(JZDataStream& s) const
{
    s << method;
    s << taskId;
    s << remoteUrl;
    s << fileList;
    s << fileMd5List;
}

void JZNetPackFileOperatorReq::loadFromStream(JZDataStream& s)
{
    s >> method;
    s >> taskId;
    s >> remoteUrl;
    s >> fileList;
    s >> fileMd5List;
}

//JZNetPackFileOperator
JZNetPackFileOperatorRes::JZNetPackFileOperatorRes() 
{
    remoteTaskId = -1;
    ok = false;
    taskId = -1;
}

JZNetPackFileOperatorRes::~JZNetPackFileOperatorRes() 
{
}

int JZNetPackFileOperatorRes::packetType() const
{
    return NetPack_fileTransRes;
}

void JZNetPackFileOperatorRes::saveToStream(JZDataStream& s) const 
{
    s << taskId;
    s << ok;
    s << remoteTaskId;
    s << error;
    s << fileList;
    s << fileMd5List;
}

void JZNetPackFileOperatorRes::loadFromStream(JZDataStream& s) 
{
    s >> taskId;
    s >> ok;
    s >> remoteTaskId;
    s >> error;
    s >> fileList;
    s >> fileMd5List;
}

//JZNetPackFileReq
JZNetPackFileReq::JZNetPackFileReq() 
{
    taskId = -1;
    remoteTaskId = -1;
    offset = 0;
    fileIndex = -1;
}

JZNetPackFileReq::~JZNetPackFileReq() 
{
}

int JZNetPackFileReq::packetType() const
{ 
    return NetPack_fileDataReq; 
}

void JZNetPackFileReq::saveToStream(JZDataStream& s) const 
{
    assert(taskId != -1 && remoteTaskId != -1);
    s << taskId;
    s << remoteTaskId;
    s << fileIndex;
    s << offset;        
}

void JZNetPackFileReq::loadFromStream(JZDataStream& s) 
{
    s >> taskId;
    s >> remoteTaskId;
    s >> fileIndex;
    s >> offset;    
}

//JZNetPackFileRes
JZNetPackFileRes::JZNetPackFileRes() 
{
    taskId = -1;
    remoteTaskId = -1;
    fileSize = -1;
    fileIndex = -1;
}

JZNetPackFileRes::~JZNetPackFileRes() 
{
}

int JZNetPackFileRes::packetType() const
{ 
    return NetPack_fileDataRes;
}

void JZNetPackFileRes::saveToStream(JZDataStream& s) const 
{
    assert(taskId != -1 && remoteTaskId != -1);
    s << taskId;
    s << remoteTaskId;
    s << buffer;
    s << fileIndex;
    s << fileSize;
}

void JZNetPackFileRes::loadFromStream(JZDataStream& s) 
{
    s >> taskId;
    s >> remoteTaskId;
    s >> buffer;
    s >> fileIndex;
    s >> fileSize;
}

//JZNetPackFileResult
JZNetPackFileResult::JZNetPackFileResult()
{
    remoteTaskId = -1;
    isFinish = false;
}

JZNetPackFileResult::~JZNetPackFileResult()
{
}

int JZNetPackFileResult::packetType() const
{
    return NetPack_fileResult;
}

void JZNetPackFileResult::saveToStream(JZDataStream& s) const
{
    s << remoteTaskId << isFinish;
}

void JZNetPackFileResult::loadFromStream(JZDataStream& s)
{
    s >> remoteTaskId >> isFinish;
}

//JZNetFileTask
JZNetFileTask::JZNetFileTask()
{    
    type = None;
    netId = INVALID_NET_HANDLE;
    taskId = -1;
    remoteTaskId = -1;
    status = Running;
    fileSize = -1;
    offset = 0;
    fileIndex = 0;
    recvTime = JZPlatform::getTickCountMs();
}

//JZNetFileTrans
bool JZNetFileTrans::isFilePack(int type)
{
    return type >= NetPack_filePacketStart && type <= NetPack_filePacketEnd;
}

void JZNetFileTrans::registPack()
{
    JZNetPackManager::instance()->registPack<JZNetPackFileOperatorReq>(NetPack_fileTransReq);
    JZNetPackManager::instance()->registPack<JZNetPackFileOperatorRes>(NetPack_fileTransRes);
    JZNetPackManager::instance()->registPack<JZNetPackFileReq>(NetPack_fileDataReq);
    JZNetPackManager::instance()->registPack<JZNetPackFileRes>(NetPack_fileDataRes);
    JZNetPackManager::instance()->registPack<JZNetPackFileResult>(NetPack_fileResult);
}

JZNetFileTrans::JZNetFileTrans()
{
	m_client = nullptr;
	m_server = nullptr;
    m_taskId = 0;
    m_eachRoundSize = 128 * 1024;
}

JZNetFileTrans::~JZNetFileTrans()
{
}

void JZNetFileTrans::setClient(JZNetClient* client)
{
	m_client = client;
}

void JZNetFileTrans::setServer(JZNetServer* server)
{
	m_server = server;
}

void JZNetFileTrans::setRootDir(std::string path)
{
    m_path = path;
}

std::string JZNetFileTrans::rootDir()
{
    return m_path;
}

std::string JZNetFileTrans::fullPath(std::string path)
{
    if (JZDir::isAbsolutePath(path))
        return path;
    else
    {
        assert(!m_path.empty());
        return JZDir(m_path).absoluteFilePath(path);        
    }
}

void JZNetFileTrans::closeConn(uint64_t net_id)
{
    auto it = m_tasks.begin();
    while (it != m_tasks.end())
    {
        if (it->second->netId == net_id)
        {
            LOG_FMT_I("FileTrans closed task = %d", it->second->taskId);

            it->second->status = JZNetFileTask::Error;
            it = m_tasks.erase(it);
        }
        else
            it++;
    }
}

bool JZNetFileTrans::recvPack(uint64_t net_id, JZNetPackPtr pack)
{    
    if (!isFilePack(pack->packetType()))
        return false;

    LOG_FMT_I("JZNetFileTrans recvPack %d", pack->packetType());
    dealPack(net_id, pack);
    return true;
}

void JZNetFileTrans::dealOpReq(uint64_t net_id, JZNetPackFileOperatorReq *req)
{
    JZNetPackFileOperatorRes res;
    JZNetFileTaskPtr task = JZNetFileTaskPtr(new JZNetFileTask());
    task->taskId = m_taskId++;
    task->remoteTaskId = req->taskId;
    task->netId = net_id;

    string remote_path = fullPath(req->remoteUrl);
    bool ret = false;
    if(!str_startsWith(remote_path,m_path)){
        goto req_end;
    }

    if (req->method == FileTaskType::Push)
    {
        task->type = FileTaskType::Pull;
        task->fileList = req->fileList;
        task->fileMd5List = req->fileMd5List;
        m_tasks[task->taskId] = task;
        ret = true;
    }
    else if (req->method == FileTaskType::Pull)
    {
        task->type = FileTaskType::Push;
        task->fileList = req->fileList;
        task->fileMd5List = req->fileMd5List;
        m_tasks[task->taskId] = task;
        ret = true;
    }
    else if (req->method == FileTaskType::PushDir)
    {
        task->type = FileTaskType::PullDir;
        task->fileList = req->fileList;
        task->fileMd5List = req->fileMd5List;
        m_tasks[task->taskId] = task;
        ret = true;
    }
    else if (req->method == FileTaskType::PullDir)
    {
        task->type = FileTaskType::PushDir;
        task->fileList = req->fileList;
        task->fileMd5List = req->fileMd5List;
        m_tasks[task->taskId] = task;
        ret = true;
    }
    else if (req->method == FileTaskType::PushDirSync)
    {
        vector<std::string> fileList;
        vector<std::string> fileMd5List;
        JZDir dir;
        if (!dir.exists(remote_path))
            dir.mkdir(remote_path);

        if (getFileList(remote_path, fileList, fileMd5List, true))
        {
            //删除无用的                
            for (size_t i = 0; i < req->fileList.size(); i++)
                req->fileList[i] = fullPath(req->fileList[i]);

            for (size_t i = 0; i < fileList.size(); i++)
            {                
                if (!vec_contains(req->fileList, fileList[i]))
                {
                    std::string local_path = fullPath(fileList[i]);
                    if (!JZFile::exists(local_path))
                        continue;

                    if (fileMd5List[i] == "dir")
                        ret = JZDir(local_path).removeRecursively();
                    else
                        ret = JZFile::remove(local_path);

                    if (!ret)
                    {
                        LOG_FMT_W("JZNetFileTrans remove file %s failed", local_path.c_str());
                        goto req_end;
                    }
                }
            }

            for (size_t remote_idx = 0; remote_idx < req->fileList.size(); remote_idx++)
            {
                int local_idx = vec_indexOf(fileList, req->fileList[remote_idx]);
                task->fileList.push_back(req->fileList[remote_idx]);
                if (local_idx != -1 && fileMd5List[local_idx] == req->fileMd5List[remote_idx])
                    task->fileMd5List.push_back("pass");
                else
                    task->fileMd5List.push_back(req->fileMd5List[remote_idx]);
            }            

            task->type = FileTaskType::PullDirSync;
            m_tasks[task->taskId] = task;
            ret = true;
        }
        else
        {
            LOG_FMT_E("FileTrans getFileList %s failed", remote_path.c_str());
            ret = false;
        }
    }
    else if (req->method == FileTaskType::PullDirSync)
    {
        assert(0);
    }
    else if (req->method == FileTaskType::CheckMd5)
    {
        JZFile file;

        string remote_md5 = str_fromUtf8(JZCrypto::fileMd5(remote_path).toHex());
        ret = (remote_md5 == req->fileMd5List[0]);
    }
    else if (req->method == FileTaskType::MkDir)
    {
        JZDir dir(m_path);
        if (!dir.exists(remote_path))
            ret = dir.mkdir(remote_path);
        else
            ret = true;

        if(!ret)
            LOG_FMT_E("FileTrans mkdir %s failed", remote_path.c_str());
    }
    else if (req->method == FileTaskType::RmDir)
    {
        JZDir dir(m_path);
        if (dir.exists(remote_path))
            ret = dir.rmdir(remote_path);
        else
            ret = true;

        if (!ret)
            LOG_FMT_E("FileTrans rmdir %s failed", remote_path.c_str());
    }
    else if (req->method == FileTaskType::RmFile)
    {
        ret = JZFile::remove(remote_path);
    }

req_end:
    res.ok = ret;
    res.taskId = task->taskId;
    res.remoteTaskId = task->remoteTaskId;
    sendPack(net_id, &res);
    if (ret && (task->type == FileTaskType::Pull
        || task->type == FileTaskType::PullDir
        || task->type == FileTaskType::PullDirSync)) //对方请求push
    {
        sendFileReq(task.get());
    }
}

void JZNetFileTrans::dealOpRes(uint64_t net_id, JZNetPackFileOperatorRes *res)
{
    int task_id = getLocalTaskId(res);
    JZNetFileTask *task = m_tasks[task_id].get();
    if (res->ok)
    {
        task->remoteTaskId = res->taskId;
        if (task->type == FileTaskType::Pull)   //对方push 或 我方pull
        {
            sendFileReq(task);
        }
        else if (task->type == FileTaskType::Push)
        {
            
        }
        else if (task->type == FileTaskType::PullDir)
        {
            sendFileReq(task);
        }
        else if (task->type == FileTaskType::PushDir)   
        {
            
        }
        else if (task->type == FileTaskType::PullDirSync)   
        {   
            assert(0);
            sendFileReq(task);
        }
        else if (task->type == FileTaskType::PushDirSync)
        {
        }
        else
        {
            //其他task直接finish
            taskFinish(task->taskId, JZNetFileTask::Finish);
        }
    }
    else
    {
        LOG_FMT_W("FileTrans remote task id = %d error", task->remoteTaskId);
        taskFinish(task->taskId, JZNetFileTask::Error, false);
    }
}

void JZNetFileTrans::dealPack(uint64_t net_id, JZNetPackPtr pack)
{
    if (pack->packetType() == NetPack_fileTransReq)
    {
        auto req = (JZNetPackFileOperatorReq*)pack.get();
        dealOpReq(net_id, req);        
    }
    else
    {
        int task_id = getLocalTaskId(pack.get());
        if (!map_contains(m_tasks, task_id))
        {
            LOG_FMT_W("FileTrans invaild task_id = %d", task_id);
            return;
        }

        JZNetFileTask *task = m_tasks[task_id].get();
        task->recvTime = JZPlatform::getTickCountMs();
        if (pack->packetType() == NetPack_fileTransRes)
        {            
            auto res = static_cast<JZNetPackFileOperatorRes*>(pack.get());
            dealOpRes(net_id, res);            
        }
        else if (pack->packetType() == NetPack_fileResult)
        {
            JZNetPackFileResult *file_ret = static_cast<JZNetPackFileResult*>(pack.get());
            if(!file_ret->isFinish)
                LOG_FMT_W("FileTrans remote task id = %d error", task->remoteTaskId);
            taskFinish(task->taskId, file_ret->isFinish? JZNetFileTask::Finish : JZNetFileTask::Error, false);
        }
        else if (pack->packetType() == NetPack_fileDataReq)
        {
            LOG_I("NetPack_fileDataReq");

            assert(task->remoteTaskId != -1);
            auto req = static_cast<JZNetPackFileReq*>(pack.get());
            
            bool ok = true;
            JZNetPackFileRes res;
            if (req->offset == 0)
            {
                assert(!task->file.isOpen());

                string filepath = task->fileList[req->fileIndex];
                filepath = fullPath(filepath);
                if (task->file.open(filepath, "rb"))
                {
                    task->fileIndex = req->fileIndex;
                    task->fileSize = task->file.size();
                    res.fileSize = task->fileSize;
                }
                else
                {
                    LOG_FMT_W("FileTrans open file %s failed", filepath.c_str());
                    ok = false;
                }
            }       
            if (ok)
            {
                res.buffer = task->file.read(m_eachRoundSize);
                task->offset += res.buffer.size();
                if (res.buffer.size() != m_eachRoundSize)
                {
                    ok = (task->offset == task->fileSize);
                    if (!ok)
                        LOG_FMT_W("FileTrans read file failed");
                }
            }
            
            if (ok)
            {                
                res.taskId = task->remoteTaskId;
                res.remoteTaskId = task->remoteTaskId;
                res.fileIndex = task->fileIndex;
                sendPack(net_id, &res);

                if (task->offset == task->fileSize)
                {
                    task->offset = 0;
                    task->fileSize = -1;
                    task->fileIndex = -1;
                    task->file.close();
                }
            }
            else
                taskFinish(task->taskId, JZNetFileTask::Error);
        }
        else if (pack->packetType() == NetPack_fileDataRes)
        {
            auto data_res = static_cast<JZNetPackFileRes*>(pack.get());

            assert(task->remoteTaskId != -1);
            assert(task->fileIndex == data_res->fileIndex);              
            if (task->offset == 0)
                task->fileSize = data_res->fileSize;
            
            task->file.write(data_res->buffer);
            task->offset += data_res->buffer.size();                
            sendFileReq(task);                        
        }
    }
}

bool JZNetFileTrans::getFileList(std::string local_dir, std::vector<std::string> &outFileList, std::vector<std::string> &outFileMd5List, bool calcMd5)
{
    JZDir dir(local_dir);    
    if (!dir.exists())
        return false;

    outFileList.push_back(local_dir);
    outFileMd5List.push_back("dir");

    auto file_list = dir.entryInfoList();
    for (int i = 0; i < (int)file_list.size(); i++)
    {        
        if (file_list[i].isDir())
        {            
            if (!getFileList(file_list[i].filePath(), outFileList, outFileMd5List, calcMd5))
                return false;
        }
        else
        {
            outFileList.push_back(file_list[i].filePath());
            string md5 = str_fromUtf8(JZCrypto::fileMd5(file_list[i].filePath()).toHex());
            if (md5.size() != 32)
                return false;

            outFileMd5List.push_back(md5);
        }
    }
    return true;
}

bool JZNetFileTrans::getTaskFileList(JZNetFileTask *task, std::string local_dir, std::string remote_dir, std::vector<std::string> &remoteFileList, bool calcMd5)
{
    std::vector<std::string> outFileList;
    std::vector<std::string> outFileMd5List;
    if (!getFileList(local_dir, outFileList, outFileMd5List, calcMd5))
        return false;
    
    task->fileList = outFileList;
    task->fileMd5List = outFileMd5List;

    int pre = (int)local_dir.size();
    for (int i = 0; i < (int)outFileList.size(); i++)
    {
        string remote = remote_dir + outFileList[i].substr(pre);
        remoteFileList.push_back(remote);
    }
    return true;
}

JZNetFileTaskPtr JZNetFileTrans::startTask(uint64_t net_id, FileTaskType task_type, std::string localUrl, std::string remoteUrl)
{
    std::string local_path = fullPath(localUrl);

    JZNetFileTaskPtr task = JZNetFileTaskPtr(new JZNetFileTask());
    task->taskId = m_taskId++;
    task->netId = net_id;
    task->type = task_type;         

    JZNetPackFileOperatorReq req;
    req.method = task_type;
    req.taskId = task->taskId;
    req.remoteUrl = remoteUrl;

    if (task_type == FileTaskType::Push)
    {
        req.fileList.push_back(remoteUrl);
        req.fileMd5List.push_back(string());

        task->fileList.push_back(local_path);
        task->fileMd5List = req.fileMd5List;
    }
    else if(task_type == FileTaskType::Pull)
    {
        req.fileList.push_back(remoteUrl);
        req.fileMd5List.push_back(string());

        task->fileList.push_back(local_path);
        task->fileMd5List = req.fileMd5List;
    }        
    else if (task_type == FileTaskType::PushDir)
    {
        std::vector<std::string> remoteFileList;
        if(!getTaskFileList(task.get(), localUrl, remoteUrl, remoteFileList, false))
            return JZNetFileTaskPtr();

        req.fileList = remoteFileList;
        req.fileMd5List = task->fileMd5List;
    }
    else if (task_type == FileTaskType::PullDir)
    {
    }
    else if (task_type == FileTaskType::PushDirSync)
    {
        std::vector<std::string> remoteFileList;
        if (!getTaskFileList(task.get(), localUrl, remoteUrl, remoteFileList, true))
            return JZNetFileTaskPtr();

        req.fileList = remoteFileList;
        req.fileMd5List = task->fileMd5List;
    }
    else if (task_type == FileTaskType::PullDirSync)
    {
    }
    else if (task_type == FileTaskType::CheckMd5)
    {
        string localFileMd5 = str_fromUtf8(JZCrypto::fileMd5(local_path).toHex());
        if (localFileMd5.empty())
            return JZNetFileTaskPtr();

        req.fileList.push_back(remoteUrl);
        req.fileMd5List.push_back(localFileMd5);
    }
    else if (task_type == FileTaskType::MkDir)
    {
    }
    else if (task_type == FileTaskType::RmDir)
    {

    }    
    else if (task_type == FileTaskType::RmFile)
    {

    }
    else
    {
        JZ_UNREACHABLE();
    }

    m_tasks[task->taskId] = task;
    sendPack(net_id, &req);
    return task;
}

JZNetFileTaskPtr JZNetFileTrans::checkMd5(uint64_t net_id, std::string localUrl, std::string remoteUrl)
{
    return startTask(net_id, FileTaskType::CheckMd5, localUrl, remoteUrl);
}

JZNetFileTaskPtr JZNetFileTrans::mkdir(uint64_t net_id, std::string remoteUrl)
{
    return startTask(net_id, FileTaskType::MkDir, string(), remoteUrl);
}

JZNetFileTaskPtr JZNetFileTrans::rmdir(uint64_t net_id, std::string remoteUrl)
{
    return startTask(net_id, FileTaskType::RmDir, string(), remoteUrl);
}

JZNetFileTaskPtr JZNetFileTrans::pushFile(uint64_t net_id,std::string localUrl, std::string remoteUrl)
{
    return startTask(net_id, FileTaskType::Push, localUrl, remoteUrl);
}

JZNetFileTaskPtr JZNetFileTrans::pullFile(uint64_t net_id, std::string remoteUrl, std::string localUrl)
{
    return startTask(net_id, FileTaskType::Pull, localUrl, remoteUrl);
}

JZNetFileTaskPtr JZNetFileTrans::pushDir(uint64_t net_id, std::string localUrl, std::string remoteUrl)
{
    return startTask(net_id, FileTaskType::PushDir, localUrl, remoteUrl);
}

JZNetFileTaskPtr JZNetFileTrans::pullDir(uint64_t net_id, std::string remoteUrl, std::string localUrl)
{
    return startTask(net_id, FileTaskType::PullDir, localUrl, remoteUrl);
}

JZNetFileTaskPtr JZNetFileTrans::pushDirSync(uint64_t net_id, std::string localUrl, std::string remoteUrl)
{
    return startTask(net_id, FileTaskType::PushDirSync, localUrl, remoteUrl);
}

JZNetFileTaskPtr JZNetFileTrans::pullDirSync(uint64_t net_id, std::string localUrl, std::string remoteUrl)
{
    return startTask(net_id, FileTaskType::PullDirSync, localUrl, remoteUrl);
}

bool JZNetFileTrans::waitTask(JZNetFileTaskPtr task)
{
    if (!task)
        return false;

    JZEventLoop event_loop;        
    task->recvTime = JZPlatform::getTickCountMs();
    while(task->status == JZNetFileTask::Running)
    {
        event_loop.processEvents();        
        
        int64_t cur_ms = JZPlatform::getTickCountMs();
        if (cur_ms - task->recvTime > 5 * 1000) //5秒无数据超时
        {
            LOG_W("FileTrans recv time out");
            taskFinish(task->taskId, JZNetFileTask::Error);
            break;
        }
    }

    return task->status == JZNetFileTask::Finish;
}

void JZNetFileTrans::sendPack(uint64_t net_id, const JZNetPack* pack)
{
	if (m_client)
		m_client->sendPacket(pack);
    else
        m_server->sendPacket(net_id, pack);
}

void JZNetFileTrans::taskFinish(int task_id, JZNetFileTask::Status status, bool send_resp)
{
    if (!map_contains(m_tasks, task_id))
        return;

    LOG_FMT_I("FileTrans task = %d end, status = %d", task_id, status);    
    auto task = m_tasks[task_id];
    if (send_resp && task->remoteTaskId != -1)
    {
        JZNetPackFileResult result;
        result.remoteTaskId = task->remoteTaskId;
        result.isFinish = (status == JZNetFileTask::Finish);
        sendPack(task->netId, &result);
    }
        
    task->status = status;
    task->file.close();        
    map_remove(m_tasks, task_id);
}

int JZNetFileTrans::getLocalTaskId(const JZNetPack* pack)
{
    int type = pack->packetType();
    if (type == NetPack_fileTransRes)
    {
        auto res = static_cast<const JZNetPackFileOperatorRes*>(pack);
        return res->remoteTaskId;
    }
    else if (type == NetPack_fileDataReq)
    {
        auto req = static_cast<const JZNetPackFileReq*>(pack);
        return req->remoteTaskId;
    }
    else if (type == NetPack_fileDataRes)
    {
        auto res = static_cast<const JZNetPackFileRes*>(pack);
        return res->remoteTaskId;
    }
    else if (type == NetPack_fileResult)
    {
        auto res = static_cast<const JZNetPackFileResult*>(pack);
        return res->remoteTaskId;
    }
    else
    {
        JZ_UNREACHABLE();
    }
}

void JZNetFileTrans::sendFileReq(JZNetFileTask* task)
{    
    if (task->offset == task->fileSize)
    {
        task->fileIndex++;
        task->offset = 0;
        task->fileSize = -1;
        task->file.close();
    }        
    
    if (task->offset == 0)
    {
        assert(!task->file.isOpen());
        while (task->fileIndex < (int)task->fileList.size())
        {            
            if (task->fileMd5List[task->fileIndex] == "pass")
            {
            }
            else if(task->fileMd5List[task->fileIndex] == "dir")
            {
                string full_path = fullPath(task->fileList[task->fileIndex]);

                JZDir dir;
                if(!dir.mkdir(full_path))
                {
                    LOG_FMT_I("FileTrans mkdir %s failed", full_path.c_str());
                    taskFinish(task->taskId, JZNetFileTask::Error);
                    return;
                }
            }
            else
            {
                break;
            }
            task->fileIndex++;
        }
        if (task->fileIndex == (int)task->fileList.size())
        {
            taskFinish(task->taskId, JZNetFileTask::Finish);
            return;
        }

        string path = task->fileList[task->fileIndex];
        path = fullPath(path);
        if (!task->file.open(path, "wb"))
        {
            LOG_FMT_I("FileTrans open %s failed", path.c_str());
            taskFinish(task->taskId, JZNetFileTask::Error);
            return;
        }
    }

    JZNetPackFileReq req;
    req.offset = task->offset;
    req.taskId = task->taskId;
    req.remoteTaskId = task->remoteTaskId;    
    req.fileIndex = task->fileIndex;
    sendPack(task->netId, &req);    
}