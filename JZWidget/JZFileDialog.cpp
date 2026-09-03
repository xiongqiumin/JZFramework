#include "JZFileDialog.h"

// 必须在 <windows.h> 之前定义 NOMINMAX, 否则 min/max 宏会与 JZStyleSet.h 等冲突。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>

#include "JZWindow.h"
#include "JZStd.h"

// 把 Qt 风格 filter ("Images (*.png *.jpg);;Text (*.txt)") 转成
// Windows 需要的 "desc\0pat\0desc\0pat\0\0" 双 \0 结尾格式。
// 括号内为通配符, 多扩展名空格分隔 -> 转成 ';' 分隔。
static std::wstring buildWinFilter(const std::string& filter)
{
    std::wstring wfilter;
    if (filter.empty()) {
        wfilter += str_to_wstr("All Files (*.*)");
        wfilter += L'\0';
        wfilter += L"*.*";
        wfilter += L'\0';
        wfilter += L'\0';
        return wfilter;
    }

    // 按 ";;" 拆分每组
    std::vector<std::string> groups;
    size_t pos = 0;
    while (pos < filter.size()) {
        size_t semi = filter.find(";;", pos);
        if (semi == std::string::npos) {
            groups.push_back(filter.substr(pos));
            break;
        }
        groups.push_back(filter.substr(pos, semi - pos));
        pos = semi + 2;
    }

    for (const auto& g : groups) {
        std::string desc = g;
        std::string pat = "*.*";
        size_t l = g.find('(');
        size_t r = g.find(')');
        if (l != std::string::npos && r != std::string::npos && r > l) {
            pat = g.substr(l + 1, r - l - 1);
            for (char& c : pat)
                if (c == ' ') c = ';';
        }
        wfilter += str_to_wstr(desc);
        wfilter += L'\0';
        wfilter += str_to_wstr(pat);
        wfilter += L'\0';
    }
    wfilter += L'\0';  // 双 \0 结尾
    return wfilter;
}

// 从 JZWidget 取所属原生窗口 HWND (无则传 nullptr)。
static HWND parentHwnd(JZWidget* parent)
{
    if (!parent)
        return nullptr;
    if (JZWindow* win = parent->window())
        return (HWND)win->nativeHandle();
    return nullptr;
}

std::string JZFileDialog::getOpenFileName(JZWidget* parent,
                                          const std::string& caption,
                                          const std::string& dir,
                                          const std::string& filter)
{
    std::wstring wfilter = buildWinFilter(filter);
    std::wstring wdir = str_to_wstr(dir);
    std::wstring wcap = str_to_wstr(caption);

    wchar_t buf[MAX_PATH] = { 0 };
    OPENFILENAMEW ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = parentHwnd(parent);
    ofn.lpstrFilter = wfilter.c_str();
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)_countof(buf);
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!wdir.empty()) ofn.lpstrInitialDir = wdir.c_str();
    if (!wcap.empty()) ofn.lpstrTitle = wcap.c_str();

    if (GetOpenFileNameW(&ofn))
        return wstr_to_str(buf);
    return "";
}

std::vector<std::string> JZFileDialog::getOpenFileNames(JZWidget* parent,
                                                       const std::string& caption,
                                                       const std::string& dir,
                                                       const std::string& filter)
{
    std::vector<std::string> result;
    std::wstring wfilter = buildWinFilter(filter);
    std::wstring wdir = str_to_wstr(dir);
    std::wstring wcap = str_to_wstr(caption);

    // 多选需要更大的缓冲 (64KB)
    const size_t bufSize = 65536;
    std::wstring buf(bufSize, L'\0');
    OPENFILENAMEW ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = parentHwnd(parent);
    ofn.lpstrFilter = wfilter.c_str();
    ofn.lpstrFile = &buf[0];
    ofn.nMaxFile = (DWORD)bufSize;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_ALLOWMULTISELECT;
    if (!wdir.empty()) ofn.lpstrInitialDir = wdir.c_str();
    if (!wcap.empty()) ofn.lpstrTitle = wcap.c_str();

    if (!GetOpenFileNameW(&ofn))
        return result;

    std::wstring dirPart(buf.c_str());
    const wchar_t* p = buf.c_str() + dirPart.size() + 1;
    if (*p == L'\0') {
        // 仅选了一个文件: buf 直接是完整路径
        result.push_back(wstr_to_str(buf.c_str()));
    } else {
        // 多个文件: 第一个串是目录, 其后是各文件名
        std::wstring base = dirPart;
        while (*p) {
            std::wstring file(p);
            result.push_back(wstr_to_str(base + L"\\" + file));
            p += file.size() + 1;
        }
    }
    return result;
}

std::string JZFileDialog::getSaveFileName(JZWidget* parent,
                                          const std::string& caption,
                                          const std::string& dir,
                                          const std::string& filter)
{
    std::wstring wfilter = buildWinFilter(filter);
    std::wstring wdir = str_to_wstr(dir);
    std::wstring wcap = str_to_wstr(caption);

    wchar_t buf[MAX_PATH] = { 0 };
    OPENFILENAMEW ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = parentHwnd(parent);
    ofn.lpstrFilter = wfilter.c_str();
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)_countof(buf);
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!wdir.empty()) ofn.lpstrInitialDir = wdir.c_str();
    if (!wcap.empty()) ofn.lpstrTitle = wcap.c_str();

    if (GetSaveFileNameW(&ofn))
        return wstr_to_str(buf);
    return "";
}

// 旧式回退 (极少数 COM 不可用场景): SHBrowseForFolderW
static std::string getExistingDirectoryLegacy(JZWidget* parent,
                                             const std::string& caption,
                                             const std::string& dir)
{
    std::wstring wdir = str_to_wstr(dir);
    std::wstring wcap = str_to_wstr(caption);

    BROWSEINFOW bi = { 0 };
    bi.hwndOwner = parentHwnd(parent);
    bi.lpszTitle = wcap.empty() ? nullptr : (LPCWSTR)wcap.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lParam = (LPARAM)wdir.c_str();

    std::string result;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH] = { 0 };
        if (SHGetPathFromIDListW(pidl, path))
            result = wstr_to_str(path);
        CoTaskMemFree(pidl);
    }
    return result;
}

std::string JZFileDialog::getExistingDirectory(JZWidget* parent,
                                               const std::string& caption,
                                               const std::string& dir)
{
    // 现代风格: IFileDialog + FOS_PICKFOLDERS (Vista+ 通用项对话框)
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool needUninit = (coHr == S_OK);   // S_FALSE=已初始化, 不可在此 Uninit

    std::string result;
    IFileDialog* pfd = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(FileOpenDialog), nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
    if (SUCCEEDED(hr) && pfd) {
        DWORD opts = 0;
        pfd->GetOptions(&opts);
        pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
        if (!caption.empty())
            pfd->SetTitle(str_to_wstr(caption).c_str());
        if (!dir.empty()) {
            IShellItem* psi = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(str_to_wstr(dir).c_str(),
                                                      nullptr, IID_PPV_ARGS(&psi))) && psi) {
                pfd->SetFolder(psi);
                psi->Release();
            }
        }

        HWND hwnd = parentHwnd(parent);
        if (SUCCEEDED(pfd->Show(hwnd))) {
            IShellItem* psiResult = nullptr;
            if (SUCCEEDED(pfd->GetResult(&psiResult)) && psiResult) {
                PWSTR pPath = nullptr;
                if (SUCCEEDED(psiResult->GetDisplayName(SIGDN_FILESYSPATH, &pPath)) && pPath) {
                    result = wstr_to_str(std::wstring(pPath));
                    CoTaskMemFree(pPath);
                }
                psiResult->Release();
            }
        }
        pfd->Release();
    } else {
        // COM 不可用 (如单元模式冲突): 退化为旧式浏览器
        result = getExistingDirectoryLegacy(parent, caption, dir);
    }

    if (needUninit)
        CoUninitialize();
    return result;
}
