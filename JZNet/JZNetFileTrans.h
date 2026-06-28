#ifndef JZNET_FILE_TRANS_H_
#define JZNET_FILE_TRANS_H_

#include "JZNetPacket.h"
#include "JZNetClient.h"
#include "JZNetServer.h"

enum FileTaskType {
    None,
    Push,    //发送数据， 我发push 或对方 pull
    Pull,    //接收数据，     
    PushDir,
    PullDir,
    PushDirSync,
    PullDirSync,
    CheckMd5,
    MkDir,
    RmDir,
    RmFile,
};

//JZNetPackFileOperatorf
class JZNetPackFileOperatorReq : public JZNetPack
{
public:
	JZNetPackFileOperatorReq();
	virtual ~JZNetPackFileOperatorReq();

	virtual int packetType() const;
	virtual void saveToStream(JZDataStream& s) const;
	virtual void loadFromStream(JZDataStream& s);

    int32_t method;
	int32_t taskId;

    std::string remoteUrl;
    std::vector<std::string> fileList;   //remote
    std::vector<std::string> fileMd5List;
};

//JZNetPackFileOperator
class JZNetPackFileOperatorRes : public JZNetPack
{
public:
    JZNetPackFileOperatorRes();
    virtual ~JZNetPackFileOperatorRes();

    virtual int packetType() const;
    virtual void saveToStream(JZDataStream& s) const;
    virtual void loadFromStream(JZDataStream& s);

    bool ok;
	int32_t taskId;          //本地id
	int32_t remoteTaskId;    //远端id        
    std::vector<std::string> fileList;
    std::vector<std::string> fileMd5List;

    std::string error;
};

//JZNetPackFileReq 总是先发请求数据，再发接收
class JZNetPackFileReq : public JZNetPack
{
public:
	JZNetPackFileReq();
	virtual ~JZNetPackFileReq();

	virtual int packetType() const;
	virtual void saveToStream(JZDataStream& s) const;
	virtual void loadFromStream(JZDataStream& s);
        
    int32_t taskId;
	int32_t remoteTaskId;
	int64_t offset;
    int32_t fileIndex;
};

//JZNetPackFileRes
class JZNetPackFileRes : public JZNetPack
{
public:
	JZNetPackFileRes();
	virtual ~JZNetPackFileRes();

	virtual int packetType() const;
	virtual void saveToStream(JZDataStream& s) const;
	virtual void loadFromStream(JZDataStream& s);
	    
    int32_t taskId;
	int32_t remoteTaskId;
    int32_t fileIndex;
    int64_t fileSize;
	JZByteArray buffer;
};

//JZNetPackFileResult
class JZNetPackFileResult : public JZNetPack
{
public:
    JZNetPackFileResult();
    virtual ~JZNetPackFileResult();

    virtual int packetType() const;
    virtual void saveToStream(JZDataStream& s) const;
    virtual void loadFromStream(JZDataStream& s);
    
    int32_t remoteTaskId;
    bool isFinish;
};

class JZNetFileTask
{
public:    
    enum Status {
        Running,
        Error,
        Finish,
    };

    JZNetFileTask();

    FileTaskType type;
    uint64_t netId;
	int32_t taskId;
	int32_t remoteTaskId;
    Status status;

	JZFile file;	
	int64_t fileSize;
	int64_t offset;    
    int64_t recvTime;

    int32_t fileIndex;
    std::vector<std::string> fileList;
    std::vector<std::string> fileMd5List;
};
typedef std::shared_ptr<JZNetFileTask> JZNetFileTaskPtr;

/*
先发送 JZNetPackFileOperatorReq 建立通道，通道建立后，总是由数据发送方发送 JZNetPackFileReq
*/
class JZNetFileTrans
{

public:
    static bool isFilePack(int type);
    static void registPack();

	JZNetFileTrans();
	~JZNetFileTrans();
	
	void setClient(JZNetClient *client);
	void setServer(JZNetServer *server);
	void setRootDir(std::string path);
    std::string rootDir();

    std::string fullPath(std::string path);
    
    JZNetFileTaskPtr checkMd5(uint64_t net_id, std::string localUrl, std::string remoteUrl);
    
    JZNetFileTaskPtr mkdir(uint64_t net_id, std::string remoteUrl);
    JZNetFileTaskPtr rmdir(uint64_t net_id, std::string remoteUrl);
	JZNetFileTaskPtr pushFile(uint64_t net_id,std::string localUrl, std::string remoteUrl);
	JZNetFileTaskPtr pullFile(uint64_t net_id,std::string remoteUrl,std::string localUrl);   
    JZNetFileTaskPtr pushDir(uint64_t net_id, std::string localUrl, std::string remoteUrl);
    JZNetFileTaskPtr pullDir(uint64_t net_id, std::string remoteUrl, std::string localUrl);    
    JZNetFileTaskPtr pushDirSync(uint64_t net_id, std::string localUrl, std::string remoteUrl); //同步，删除远端多余的文件
    JZNetFileTaskPtr pullDirSync(uint64_t net_id, std::string remoteUrl, std::string localUrl);
    
    bool waitTask(JZNetFileTaskPtr task);
    void closeConn(uint64_t net_id);
	bool recvPack(uint64_t net_id,JZNetPackPtr pack);    

protected:	
    int getLocalTaskId(const JZNetPack* pack);
	void sendPack(uint64_t net_id, const JZNetPack* pack);	
    void sendFileReq(JZNetFileTask* task);

    void dealPack(uint64_t net_id, JZNetPackPtr pack);
    void dealOpReq(uint64_t net_id, JZNetPackFileOperatorReq *req);
    void dealOpRes(uint64_t net_id, JZNetPackFileOperatorRes *res);

	JZNetFileTaskPtr startTask(uint64_t net_id, FileTaskType req_type,std::string localUrl, std::string remoteUrl);
    void taskFinish(int task_id, JZNetFileTask::Status status, bool send_error = true);
    bool getFileList(std::string dir,std::vector<std::string> &fileList, std::vector<std::string> &fileMd5List,bool calcMd5);
    bool getTaskFileList(JZNetFileTask *task, std::string dir, std::string remote_dir, std::vector<std::string> &remoteFileList, bool calcMd5);

	std::map<int,JZNetFileTaskPtr> m_tasks;
	JZNetServer* m_server;
	JZNetClient* m_client;
	std::string m_path;
	int m_taskId;
	int m_eachRoundSize;
};

#endif
