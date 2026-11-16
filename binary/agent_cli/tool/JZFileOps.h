#ifndef JZ_FILE_OPS_H_
#define JZ_FILE_OPS_H_

#include <cstdint>
#include <string>

#include "JZJson.h"

// ---------------------------------------------------------------------------
// 文件树 / 目录选择器用的文件操作 —— **一份实现, 两处使用**:
//   ① server 的 /api/fs*(目标 agent 跑在本机: 内置 agent / inproc cli)
//   ② cli 收到控制面转发的 fsReq(目标 agent 在别的机器上, 只能在它本机执行)
//
// 边界模型(与网页上的一致):
//   读 —— 全盘可读(能列、能读任意路径, 只要进程有 OS 权限);
//   写 —— 只允许 root 之内(root 由调用方给, 就是网页上那个"工作区")。
//   ⚠ 这不是安全边界, 只防误操作; 真正的权限由 OS 决定(见 JZVirtualTerminalConfig.h)。
//
// 读文件有 maxBytes 上限: 控制面单个包上限 256KB, 所以远端读文件必须截断
// (调用方传 160KB 左右, truncated 告诉前端"只给了一段")。
// ---------------------------------------------------------------------------
namespace JZFileOps
{
// 列目录。path 为空 → 用 defaultDir(再空则进程当前目录)。
// 成功返回 {path, parent, dirs:[{name,path}], files:[{name,path,size,mtime}], drives:[...], home}
// 失败返回 null 并填 error(文案与以前 /api/fs 一致: "目录不存在: ...")。
Json::Value list(const std::string &path, const std::string &defaultDir, std::string &error);

// 读文件原始字节(最多 maxBytes)。二进制安全。truncated = 文件比 maxBytes 大。
bool readFile(const std::string &path, int64_t maxBytes, std::string &data,
              int64_t &size, int64_t &mtime, bool &truncated, std::string &error);

// 新建/改名/删除: 目标必须在 root 之内(root 本身也算)。
bool create(const std::string &root, const std::string &dir, const std::string &name,
            bool isDir, std::string &outPath, std::string &error);
bool renamePath(const std::string &root, const std::string &path, const std::string &name,
                std::string &outPath, std::string &error);
bool remove(const std::string &root, const std::string &path, std::string &error);

// 名字必须是单层(不含分隔符, 不是 . / ..)
bool validEntryName(const std::string &name);
// p 是否落在 root 之内(root 本身也算)
bool pathInRoot(const std::string &root, const std::string &p);

} // namespace JZFileOps

#endif // !JZ_FILE_OPS_H_
