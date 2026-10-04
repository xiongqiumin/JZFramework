#ifndef JZ_BUNDLE_PATHS_H_
#define JZ_BUNDLE_PATHS_H_

#include <string>
#include <vector>

#include "JZFileInfo.h"
#include "JZPlatform.h"

// ---------------------------------------------------------------------------
// 随包产物定位：js_wrapper/(类型 + 运行期 JS) / code_runtime.exe / ts_compiler/
//
// 正常情况：这些就摆在 exe 同目录 —— binary/ 下的**发布** exe 统一输出到
// <build>/binary/bin/<配置>/（见 binary/CMakeLists.txt），其中：
//   · js_wrapper/        一棵与源码 JZJSEngine/js_wrapper **同构**的部署树
//                        （types/ + <包>/types/ + <包>/dist/），由 copy_jz_js 铺进去；
//   · code_runtime.exe / ts_compiler/  直接躺在 exe 根。
//
// 但链接了 code_agent_core 的**测试 / 评测** exe 有自己的输出目录
// （binary/code_agent/test/<配置>/ 与 binary/code_agent/benchmark/<配置>/，见各自的
// CMakeLists —— 它们刻意不进那个发布目录），它们旁边并没有这些产物 ⇒ 除 exe 目录外，
// 还得沿上级目录把 binary/bin/<配置> 找回来。
//
// 用法：jzFindBundleAsset("js_wrapper", true)（要那棵树的根）/
//       jzFindBundleAsset("code_runtime.exe")。
//   rel 一律用 '/' 分隔；先试 <dir>/js_wrapper/<rel>，再试 <dir>/<rel>（兼容平铺布局）。
//   找不到返回空串，由调用点决定是"跳过"还是"报错"。
// ---------------------------------------------------------------------------

inline std::string jzNormBundlePath(const std::string &path)
{
    std::string p = path;
    for (size_t i = 0; i < p.size(); i++)
    {
        if (p[i] == '\\')
            p[i] = '/';
    }
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    return p;
}

// 候选目录（按优先级）：
//   ① exe 同目录（exe 本身就在发布目录里时命中）
//   ② 沿上级目录找 <祖先>/binary/bin/<配置>，以及单配置生成器的 <祖先>/binary/bin
//   ③ 更早的布局 <祖先>/binary/code_agent/bin[/<配置>]（旧构建目录还能直接跑）
inline std::vector<std::string> jzBundleDirCandidates()
{
    std::vector<std::string> dirs;
    std::string appDir = jzNormBundlePath(JZPlatform::applicationDirPath());
    if (appDir.empty())
        return dirs;

    // 多配置生成器(VS)下 exe 目录名就是配置名(Debug/Release)；单配置下是 "test"/"benchmark"，无害。
    std::string cfg = appDir.substr(appDir.find_last_of('/') + 1);
    dirs.push_back(appDir);
    std::string p = appDir;
    for (int depth = 0; depth < 8; depth++)
    {
        size_t pos = p.find_last_of('/');
        if (pos == std::string::npos || pos == 0)
            break;
        p = p.substr(0, pos);
        dirs.push_back(p + "/binary/bin/" + cfg);
        dirs.push_back(p + "/binary/bin");
        dirs.push_back(p + "/binary/code_agent/bin/" + cfg);
        dirs.push_back(p + "/binary/code_agent/bin");
    }
    return dirs;
}

// 在候选目录里找 rel；wantDir=true 表示要找目录。命中返回归一化后的绝对路径，否则空串。
inline std::string jzFindBundleAsset(const std::string &rel, bool wantDir = false)
{
    std::vector<std::string> dirs = jzBundleDirCandidates();
    for (size_t i = 0; i < dirs.size(); i++)
    {
        std::string s = dirs[i] + "/js_wrapper/" + rel;
        JZFileInfo info(s);
        if (wantDir ? info.isDir() : info.exists())
            return s;
        s = dirs[i] + "/" + rel;                 // 平铺布局(exe 根 / 旧部署)
        JZFileInfo info2(s);
        if (wantDir ? info2.isDir() : info2.exists())
            return s;
    }
    return std::string();
}

#endif // JZ_BUNDLE_PATHS_H_
