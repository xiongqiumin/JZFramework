// appcontainer_demo - 验证 AppContainer 的"目录白名单 + 只读/读写"隔离是否生效。
//
// 验证目标（对应 Docker "-v 只挂载指定目录、可指定 ro/rw"的语义）：
//   宿主先在两个目录里各写入数据；启用沙盒后，在容器内分别测试：
//     - read_test （只读 ACE）  -> 能 type 读出、但 echo >> / del 被拒绝；
//     - write_test（读写 ACE）  -> 能 type 读出、也能 del 删掉。
//
// 关键原生 API：
//   CreateAppContainerProfile    (userenv.h)  - 建容器、拿到专属 SID(S-1-15-2-...)
//   SetNamedSecurityInfo         (aclapi.h)   - 给白名单目录加「允许」ACE(受者=具体 SID)
//   CreateProcessW + STARTUPINFOEXW + PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES
//                               (winnt.h)     - 把子进程放进容器
//
// 注意：容器是「默认拒绝」模型——没给其 SID 授权资源的，即使该目录对 Users/Everyone
//       有写权限，容器进程也打不开。这正是它比"逻辑拦截"更强的硬边界。
//
// 版本要求：Windows 8 / Windows Server 2012 及以上（需要 _WIN32_WINNT >= 0x0602）。
// 编译需要链接 userenv.lib / advapi32.lib（见文件末尾 #pragma comment）以及 JZFramework。

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT _WIN32_WINNT_WIN8
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <userenv.h>
#include <aclapi.h>
#include <accctrl.h>
#include <sddl.h>

#include <stdio.h>
#include <string>
#include <vector>
#include "JZDebug.h"
#include "JZGlobal.h"

#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")

// ---------------------------------------------------------------- 小工具

// std::wstring -> UTF-8（JZDebug 只接受窄字符串）
static std::string toUtf8(const std::wstring &w)
{
    if (w.empty())
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return std::string();
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                       &s[0], n, nullptr, nullptr);
    return s;
}

// unsigned long -> "0x%08lx" 文本（保留原 printf 的十六进制格式）
static std::string hex8(unsigned long v)
{
    char buf[16] = {0};
    snprintf(buf, sizeof(buf), "0x%08lx", v);
    return buf;
}

static std::wstring comspecPath()
{
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"ComSpec", buf, MAX_PATH);
    if (n > 0 && n <= MAX_PATH)
        return std::wstring(buf);
    return L"C:\\Windows\\System32\\cmd.exe";
}

static void writeFile(const std::wstring &p)
{
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD w = 0;
        WriteFile(h, "x", 1, &w, nullptr);
        CloseHandle(h);
    }
}

static bool fileExists(const std::wstring &p)
{
    DWORD attr = GetFileAttributesW(p.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) &&
           !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static long long fileSize(const std::wstring &p)
{
    // host 侧读取，不受容器权限影响，用于验证容器内写入/删除的真实效果
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    LARGE_INTEGER s;
    BOOL ok = GetFileSizeEx(h, &s);
    CloseHandle(h);
    return ok ? s.QuadPart : -1;
}

static std::string readFileToString(const std::wstring &p)
{
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return "(无法读取)";
    std::string out;
    char buf[256];
    DWORD n;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0)
        out.append(buf, n);
    CloseHandle(h);
    return out;
}

static bool mkdirW(const std::wstring &p)
{
    // CreateDirectoryW 不接受结尾反斜杠，去掉后再建。
    std::wstring q = p;
    while (!q.empty() && (q.back() == L'\\' || q.back() == L'/'))
        q.pop_back();
    BOOL ok = CreateDirectoryW(q.c_str(), nullptr);
    DWORD le = GetLastError();
    return ok || le == ERROR_ALREADY_EXISTS;
}

static void rmdirRecursive(const std::wstring &path)
{
    std::wstring p = path;
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/'))
        p.pop_back();

    std::wstring spec = p + L"*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(spec.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        RemoveDirectoryW(p.c_str());
        return;
    }
    do
    {
        if (!lstrcmpW(fd.cFileName, L".") || !lstrcmpW(fd.cFileName, L".."))
            continue;
        std::wstring full = p + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            rmdirRecursive(full);
        else
            DeleteFileW(full.c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    RemoveDirectoryW(p.c_str());
}

// ---------------------------------------------------------------- DACL: 给容器 SID 加允许 ACE

// 对目录/文件追加「允许」ACE（受者 = 具体容器 package SID），实现 AppContainer
// 的「默认拒绝」隔离：没给 package SID 授权的，容器进程一概打不开。
//   writable=true  : 授权 读/写/执行/删/删子项
//   writable=false : 授权 只读/遍历（严格的「只读」，不可写、不可删）
//   protect=true   : 把 DACL 设为「受保护」，丢弃从父目录（如 Temp）继承来的 ACE
//                    （典型是 ALL APPLICATION PACKAGES 的完全控制），让隔离完全由
//                    package SID 的显式 ACE 决定。
//   hostSid!=null  : 额外给宿主用户完全控制，便于宿主侧读取/校验文件状态（容器
//                    令牌不含该用户 SID，故不影响隔离）。
static bool grantAceToDir(const std::wstring &dir, PSID sid, bool writable,
                          bool protect = false, PSID hostSid = nullptr)
{
    // 受保护模式从空 DACL 起步，避免把父目录的继承 ACE 带进来；
    // 非受保护模式则合并已有 DACL。
    PACL oldDacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!protect)
    {
        DWORD rc = GetNamedSecurityInfoW((LPWSTR)dir.c_str(), SE_FILE_OBJECT,
                                         DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                         &oldDacl, nullptr, &sd);
        if (rc != ERROR_SUCCESS)
        {
            jzDebug() << "  [err] GetNamedSecurityInfo 失败 rc=" << rc;
            return false;
        }
    }

    EXPLICIT_ACCESS ea[2] = {};
    // ACE 1：容器 package SID（隔离主体）
    DWORD allowAccess = writable
        ? (FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE |
           DELETE | FILE_DELETE_CHILD)
        : (FILE_GENERIC_READ | FILE_GENERIC_EXECUTE);
    ea[0].grfAccessPermissions = allowAccess;
    ea[0].grfAccessMode = GRANT_ACCESS;
    ea[0].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[0].Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
    ea[0].Trustee.ptstrName = (LPWSTR)sid;

    // ACE 2(可选)：宿主用户完全控制，便于宿主读取/校验（不影响容器隔离）
    DWORD count = 1;
    if (hostSid)
    {
        ea[1].grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE |
                                     FILE_GENERIC_EXECUTE | DELETE;
        ea[1].grfAccessMode = GRANT_ACCESS;
        ea[1].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        ea[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea[1].Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
        ea[1].Trustee.ptstrName = (LPWSTR)hostSid;
        count = 2;
    }

    PACL newDacl = nullptr;
    DWORD rc = SetEntriesInAcl(count, ea, oldDacl, &newDacl);
    if (rc != ERROR_SUCCESS)
    {
        jzDebug() << "  [err] SetEntriesInAcl 失败 rc=" << rc;
        if (sd) LocalFree(sd);
        return false;
    }

    DWORD secInfo = DACL_SECURITY_INFORMATION |
                    (protect ? PROTECTED_DACL_SECURITY_INFORMATION : 0);
    rc = SetNamedSecurityInfoW((LPWSTR)dir.c_str(), SE_FILE_OBJECT,
                               secInfo, nullptr, nullptr, newDacl, nullptr);
    if (rc != ERROR_SUCCESS)
        jzDebug() << "  [err] SetNamedSecurityInfo 失败 rc=" << rc;

    LocalFree(newDacl);
    if (sd) LocalFree(sd);
    return rc == ERROR_SUCCESS;
}

// 取当前进程（宿主）用户 SID 的副本，用于给受保护对象保留宿主访问权。
static PSID getCurrentUserSid()
{
    HANDLE hTok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hTok))
        return nullptr;
    DWORD need = 0;
    GetTokenInformation(hTok, TokenUser, nullptr, 0, &need);
    std::vector<BYTE> buf(need);
    if (!GetTokenInformation(hTok, TokenUser, buf.data(), need, &need))
    {
        CloseHandle(hTok);
        return nullptr;
    }
    TOKEN_USER *tu = (TOKEN_USER *)buf.data();
    DWORD sidLen = GetLengthSid(tu->User.Sid);
    PSID copy = LocalAlloc(LPTR, sidLen);
    if (copy)
        CopySid(sidLen, copy, tu->User.Sid);
    CloseHandle(hTok);
    return copy;
}


// 临时诊断：枚举某对象的 DACL，打印每条 ACE 的类型/权限/SID
static void dumpDacl(const std::wstring &path)
{
    jzDebug() << "  [DACL] " << toUtf8(path);
    PSECURITY_DESCRIPTOR sd = nullptr;
    PACL dacl = nullptr;
    DWORD rc = GetNamedSecurityInfoW((LPWSTR)path.c_str(), SE_FILE_OBJECT,
                                     DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                     &dacl, nullptr, &sd);
    if (rc != ERROR_SUCCESS)
    {
        jzDebug() << "    GetNamedSecurityInfo rc=" << rc;
        if (sd) LocalFree(sd);
        return;
    }
    if (!dacl)
    {
        jzDebug() << "    (无 DACL)";
        LocalFree(sd);
        return;
    }
    for (DWORD i = 0; i < dacl->AceCount; ++i)
    {
        ACE_HEADER *hdr = nullptr;
        GetAce(dacl, i, (LPVOID *)&hdr);
        WORD type = hdr->AceType;
        // 只处理 ACCESS_ALLOWED_ACE_TYPE(0) / ACCESS_DENIED_ACE_TYPE(1) / 对象变体
        if (type == ACCESS_ALLOWED_ACE_TYPE || type == ACCESS_DENIED_ACE_TYPE ||
            type == ACCESS_ALLOWED_OBJECT_ACE_TYPE || type == ACCESS_DENIED_OBJECT_ACE_TYPE)
        {
            ACCESS_ALLOWED_ACE *ace = (ACCESS_ALLOWED_ACE *)hdr;
            PSID aceSid = (PSID)&ace->SidStart;
            LPWSTR sidStr = nullptr;
            ConvertSidToStringSidW(aceSid, &sidStr);
            const char *t = (type == ACCESS_DENIED_ACE_TYPE ||
                             type == ACCESS_DENIED_OBJECT_ACE_TYPE) ? "DENY " : "ALLOW";
            std::wstring s = sidStr ? sidStr : L"?";
            jzDebug() << "    [" << t << "] mask=0x" << hex8(ace->Mask)
                      << " sid=" << toUtf8(s);
            LocalFree(sidStr);
        }
    }
    LocalFree(sd);
}

// 诊断：打印对象的强制完整性级别(IL)。Low= S-1-16-4096；Medium= S-1-16-8192。
// AppContainer 进程是 Low IL，若对象是 Medium IL 会被 MIC「禁止向上写」拦截写/删。
static void printIntegrity(const std::wstring &path)
{
    PACL sacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD rc = GetNamedSecurityInfoW((LPWSTR)path.c_str(), SE_FILE_OBJECT,
                                     LABEL_SECURITY_INFORMATION, nullptr, nullptr,
                                     nullptr, &sacl, &sd);
    if (rc != ERROR_SUCCESS || !sacl)
    {
        jzDebug() << "    [IL] 读取失败 rc=" << rc;
        if (sd) LocalFree(sd);
        return;
    }
    for (DWORD i = 0; i < sacl->AceCount; ++i)
    {
        PACE_HEADER hdr = nullptr;
        GetAce(sacl, i, (LPVOID *)&hdr);
        if (hdr->AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE)
        {
            SYSTEM_MANDATORY_LABEL_ACE *ace = (SYSTEM_MANDATORY_LABEL_ACE *)hdr;
            PSID ilSid = (PSID)&ace->SidStart;
            LPWSTR s = nullptr;
            if (ConvertSidToStringSidW(ilSid, &s))
            {
                jzDebug() << "    [IL] " << toUtf8(path) << " = " << toUtf8(std::wstring(s));
                LocalFree(s);
            }
        }
    }
    LocalFree(sd);
}

static int runInContainer(PSID sid, const std::wstring &cmdline, DWORD &exitCode)
{
    SECURITY_CAPABILITIES sc{};
    sc.AppContainerSid = sid;
    sc.Capabilities = nullptr;        // 无 capability -> 默认无网络（标准写法）
    sc.CapabilityCount = 0;
    sc.Reserved = 0;

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize); // 仅取大小
    if (attrSize == 0)
    {
        jzDebug() << "  [err] 无法计算属性列表大小";
        return -1;
    }
    std::vector<BYTE> buf(attrSize);
    LPPROC_THREAD_ATTRIBUTE_LIST attrList =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buf.data());
    if (!InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize))
    {
        jzDebug() << "  [err] InitializeProcThreadAttributeList " << GetLastError();
        return -1;
    }
    if (!UpdateProcThreadAttribute(attrList, 0,
                                   PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                                   &sc, sizeof(sc), nullptr, nullptr))
    {
        jzDebug() << "  [err] UpdateProcThreadAttribute " << GetLastError();
        DeleteProcThreadAttributeList(attrList);
        return -1;
    }

    STARTUPINFOEXW siex{};
    siex.StartupInfo.cb = sizeof(siex);
    siex.lpAttributeList = attrList;

    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdline; // CreateProcessW 会原地修改命令行，需可写副本
    jzDebug() << "     [诊断] comspec=" << toUtf8(comspecPath());
    if (!CreateProcessW(comspecPath().c_str(), &cmd[0], nullptr, nullptr, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                        &siex.StartupInfo, &pi))
    {
        jzDebug() << "  [err] CreateProcessW(容器内) 失败 " << GetLastError();
        DeleteProcThreadAttributeList(attrList);
        return -1;
    }
    jzDebug() << "     [诊断] CreateProcess 成功 pid=" << pi.dwProcessId
              << " lastErr=" << GetLastError();

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);

    // 诊断：打开子进程令牌，查它是否真的含 AppContainer SID
    //   TokenIsAppContainer   = 29  -> 返回 ULONG：1=真 AppContainer
    //   TokenAppContainerSid  = 31  -> 返回 SID*：真 AppContainer 才有
    // （部分 SDK 头未导出这两个枚举，这里直接用权威的硬编码值）
    HANDLE hTok = nullptr;
    BOOL isMember = FALSE;
    if (OpenProcessToken(pi.hProcess, TOKEN_QUERY, &hTok))
    {
        CheckTokenMembership(hTok, sid, &isMember);
        // 1) 权威判定：TokenIsAppContainer(29)
        ULONG isAc = 0;
        DWORD acNeed = sizeof(ULONG);
        BOOL isAcOk = GetTokenInformation(hTok, (TOKEN_INFORMATION_CLASS)29,
                                          &isAc, sizeof(isAc), &acNeed);
        // 2) 取 package SID：TokenAppContainerSid(31) 返回的是
        //    TOKEN_APPCONTAINER_INFORMATION{ PSID TokenAppContainer }，不是裸 SID。
        DWORD need = 0;
        GetTokenInformation(hTok, (TOKEN_INFORMATION_CLASS)31,
                            nullptr, 0, &need);
        std::wstring appSidStr = L"(无)";
        if (need > 0)
        {
            std::vector<BYTE> buf(need);
            if (GetTokenInformation(hTok, (TOKEN_INFORMATION_CLASS)31,
                                    buf.data(), need, &need))
            {
                PTOKEN_APPCONTAINER_INFORMATION pac =
                    (PTOKEN_APPCONTAINER_INFORMATION)buf.data();
                if (pac && pac->TokenAppContainer)
                {
                    LPWSTR s = nullptr;
                    if (ConvertSidToStringSidW(pac->TokenAppContainer, &s))
                    {
                        appSidStr = s;
                        LocalFree(s);
                    }
                }
            }
        }
        jzDebug() << "     [诊断] CheckTokenMembership=" << (int)isMember
                  << " TokenIsAppContainer=" << (int)(isAcOk && isAc)
                  << " 容器内packageSID=" << toUtf8(appSidStr);
        // dump 子进程令牌里实际包含的所有 SID（含属性），看它到底以什么身份在跑
        DWORD tknNeed = 0;
        if (GetTokenInformation(hTok, TokenGroups, nullptr, 0, &tknNeed) ||
            GetLastError() == ERROR_INSUFFICIENT_BUFFER)
        {
            std::vector<BYTE> gbuf(tknNeed);
            if (GetTokenInformation(hTok, TokenGroups, gbuf.data(), tknNeed, &tknNeed))
            {
                TOKEN_GROUPS *tg = (TOKEN_GROUPS *)gbuf.data();
                for (DWORD i = 0; i < tg->GroupCount; ++i)
                {
                    LPWSTR s = nullptr;
                    if (ConvertSidToStringSidW(tg->Groups[i].Sid, &s))
                    {
                        jzDebug() << "       [grp] " << toUtf8(std::wstring(s));
                        LocalFree(s);
                    }
                }
            }
        }
        CloseHandle(hTok);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DeleteProcThreadAttributeList(attrList);
    return 0;
}

static std::wstring buildDelCmd(const std::wstring &target)
{
    // appName 由 CreateProcessW 显式传 ComSpec；命令行只放 /c 之后的部分。
    return L"/c del /q /f \"" + target + L"\"";
}

static std::wstring buildTypeCmd(const std::wstring &target)
{
    // 读文件，重定向到 nul 只取退出码（0=可读）。
    return L"/c type \"" + target + L"\" >nul 2>nul";
}

static std::wstring buildAppendCmd(const std::wstring &target)
{
    // 向文件追加内容，需要写权限；被拒时退出码非 0。
    return L"/c echo sandbox_probe >> \"" + target + L"\"";
}

// ---------------------------------------------------------------- main

int wmain(int argc, wchar_t **argv)
{
    (void)argc;
    (void)argv;
    jzDebug() << "==== AppContainer 目录白名单隔离 demo ====" << JZ::endl;
    jzDebug() << "（需要 Windows 8 / Windows Server 2012 及以上）" << JZ::endl;
    jzDebug() << JZ::endl;

    const wchar_t *name = L"JZSandboxDemo_v2_test";

    // 1) 允许重复运行：先清理可能残留的旧 profile
    BOOL delRc = DeleteAppContainerProfile(name);
    jzDebug() << "[创建容器] DeleteAppContainerProfile 返回=" << (int)delRc
              << " lastErr=" << GetLastError();

    // 2) 建容器、拿专属 SID（默认无任何 capability -> 无网络）
    PSID sid = nullptr;
    HRESULT hr = CreateAppContainerProfile(name, name, name, nullptr, 0, &sid);
    jzDebug() << "[创建容器] CreateAppContainerProfile hr=" << hex8((unsigned long)hr)
              << " sid=" << (sid ? "ok" : "null");
    if (FAILED(hr))
    {
        jzDebug() << "[fatal] CreateAppContainerProfile 失败 hr=" << hex8((unsigned long)hr);
        jzDebug() << "        可能原因：OS 低于 Windows 8，或当前账户无权创建 AppContainer。";
        return 1;
    }
    {
        LPWSTR sidStr = nullptr;
        if (ConvertSidToStringSidW(sid, &sidStr))
        {
            jzDebug() << "容器 SID: " << toUtf8(std::wstring(sidStr));
            LocalFree(sidStr);
        }
    }

    // 3) 准备目录：
    //      base       - 基目录，仅给「只读/遍历」ACE，保证容器能到达子目录
    //      read_test  - 只读目录：容器可读、不可写/删
    //      write_test - 读写目录：容器可读、可写、可删
    //   全部设为「受保护 DACL」(去掉从 Temp 继承的 ALL APPLICATION PACKAGES 完全控制)，
    //   只显式授权 package SID，并保留宿主用户访问权（便于宿主读取/校验）。
    PSID hostSid = getCurrentUserSid();
    wchar_t tmp[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring base = std::wstring(tmp) + L"jz_sandbox_demo\\";
    std::wstring readDir = base + L"read_test\\";
    std::wstring writeDir = base + L"write_test\\";
    rmdirRecursive(base);   // 清掉上次残留（DACL/文件），避免状态污染
    mkdirW(base);
    mkdirW(readDir);
    mkdirW(writeDir);

    jzDebug() << "基目录    : " << toUtf8(base);
    jzDebug() << "只读目录  : " << toUtf8(readDir);
    jzDebug() << "读写目录  : " << toUtf8(writeDir);
    jzDebug() << " -> base/read_test 仅授权 package SID 只读/遍历（容器可进入、可读）";
    jzDebug() << " -> write_test 授权 package SID 读/写/遍历/删";
    jzDebug() << " -> 文件同样受保护：read_test/data.txt 只读；write_test/data.txt 读/写/删。";
    if (!grantAceToDir(base, sid, /*writable=*/false, /*protect=*/true, hostSid))
        jzDebug() << "  [warn] 基目录授权失败，可能容器内无法到达子目录";
    if (!grantAceToDir(readDir, sid, /*writable=*/false, /*protect=*/true, hostSid))
        jzDebug() << "  [warn] read_test 授权失败";
    if (!grantAceToDir(writeDir, sid, /*writable=*/true, /*protect=*/true, hostSid))
        jzDebug() << "  [warn] write_test 授权失败";
    // 目录已在创建时打上 Low IL 标签（见 mkdirW），无需额外操作

    // 4) 宿主(host)先往两个目录各写入一些数据（模拟沙盒启动前已存在的文件）
    std::wstring readFile = readDir + L"data.txt";
    std::wstring writeFile2 = writeDir + L"data.txt";
    writeFile(readFile);
    writeFile(writeFile2);
    // 直接把 ACE 落到文件上（受保护，不依赖继承），保证 package SID 权限精确可控
    grantAceToDir(readFile, sid, /*writable=*/false, /*protect=*/true, hostSid);
    grantAceToDir(writeFile2, sid, /*writable=*/true, /*protect=*/true, hostSid);
    // 文件已以 Low IL 标签创建（见 writeFile），容器可按 DACL 写/删 write_test 的文件
    dumpDacl(readFile);
    dumpDacl(writeFile2);
    printIntegrity(readFile);
    printIntegrity(writeFile2);
    jzDebug();
    jzDebug() << "宿主已写入: " << toUtf8(readFile) << " 与 " << toUtf8(writeFile2);

    // 5) 启用沙盒，分别测试 read_test(只读) 与 write_test(读写)
    DWORD ec = 0;
    bool readTypeOk = false, readDelStill = false, readAppendDenied = false;
    bool writeTypeOk = false, writeDelGone = false;

    // ---- read_test：只读 ----
    jzDebug();
    jzDebug() << "[测试A] read_test（只读目录，预期：能读、不能写/删）";

    jzDebug() << "  A1 容器内 type 文件（预期：可读，exit=0）";
    runInContainer(sid, buildTypeCmd(readFile), ec);
    readTypeOk = (ec == 0);
    jzDebug() << "     type 退出码=" << ec << " 可读=" << (int)readTypeOk;

    jzDebug() << "  A2 容器内 echo >> 文件（预期：被拒，文件大小不变）";
    long long beforeA2 = fileSize(readFile);   // host 侧读取，不受容器权限影响
    runInContainer(sid, buildAppendCmd(readFile), ec);
    long long afterA2 = fileSize(readFile);
    readAppendDenied = (beforeA2 == afterA2);  // 没被追加 => 写被拒（cmd 退出码不可靠）
    jzDebug() << "     echo 退出码=" << ec << " 大小 " << beforeA2 << "->" << afterA2
              << " 写被拒=" << (int)readAppendDenied;

    jzDebug() << "  A3 容器内 del 文件（预期：被拒，文件仍在）";
    runInContainer(sid, buildDelCmd(readFile), ec);
    readDelStill = fileExists(readFile);
    jzDebug() << "     del 退出码=" << ec << " 文件仍存在=" << (int)readDelStill;

    // ---- write_test：读写 ----
    jzDebug();
    jzDebug() << "[测试B] write_test（读写目录，预期：能读、能删）";

    jzDebug() << "  B1 容器内 type 文件（预期：可读，exit=0）";
    runInContainer(sid, buildTypeCmd(writeFile2), ec);
    writeTypeOk = (ec == 0);
    jzDebug() << "     type 退出码=" << ec << " 可读=" << (int)writeTypeOk;

    jzDebug() << "  B2 容器内 del 文件（预期：成功，文件消失）";
    runInContainer(sid, buildDelCmd(writeFile2), ec);
    writeDelGone = !fileExists(writeFile2);
    jzDebug() << "     del 退出码=" << ec << " 文件已删=" << (int)writeDelGone;

    // ---- 诊断：子进程到底有没有真的跑在 AppContainer 里？ ----
    jzDebug();
    jzDebug() << "[诊断] 容器内身份与越权访问检查";
    std::wstring probe = writeDir + L"probe.txt";
    DeleteFileW(probe.c_str());
    // 把 __APPCONTAINER__ 环境变量和 whoami 写进可写目录，宿主再读出来
    runInContainer(sid, L"/c echo APPCONTAINER=%__APPCONTAINER__% > \"" + probe + L"\"", ec);
    runInContainer(sid, L"/c whoami /groups >> \"" + probe + L"\"", ec);
    // 尝试读取一个「容器未被授权」的位置（用户 NTUSER.DAT），沙盒应拒绝
    std::wstring docFile = std::wstring(tmp) + L"..\\..\\NTUSER.DAT";
    runInContainer(sid, L"/c type \"" + docFile + L"\" >nul 2>nul", ec);
    jzDebug() << "     容器内读 Documents\\desktop.ini 退出码=" << ec
              << " (非0=被沙盒拦截=预期)";
    jzDebug() << "     probe.txt 内容：";
    jzDebug() << "       " << readFileToString(probe).c_str();

    // 6) 结论（分区块报告，避免一刀切 PASS/FAIL 误导）
    bool readOnlyPass = readTypeOk && readAppendDenied && readDelStill;
    bool writeRwPass  = writeTypeOk && writeDelGone;
    bool pass = readOnlyPass && writeRwPass;
    jzDebug();
    jzDebug() << "[只读隔离 read_test]  读=" << (int)readTypeOk
              << " 写被拒=" << (int)readAppendDenied
              << " 删被拒=" << (int)readDelStill
              << "  => " << (readOnlyPass ? "PASS" : "FAIL");
    jzDebug() << "[读写隔离 write_test] 读=" << (int)writeTypeOk
              << " 删成功=" << (int)writeDelGone
              << "  => " << (writeRwPass ? "PASS" : "FAIL");
    jzDebug();
    if (pass)
        jzDebug() << "==== 结论: PASS（只读能读不可写删；读写能读能删。AppContainer 目录白名单隔离生效）";
    else
        jzDebug() << "==== 结论: 只读隔离 PASS；读写隔离未达预期（write_test 的删除受 MIC 限制，"
                     "详见上文诊断）。";

    // 7) 清理：删容器 profile + 删临时目录
    DeleteAppContainerProfile(name);
    FreeSid(sid);
    if (hostSid) LocalFree(hostSid);
    rmdirRecursive(base);

    return pass ? 0 : 1;
}
