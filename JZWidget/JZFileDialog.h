#ifndef JZ_FILE_DIALOG_H_
#define JZ_FILE_DIALOG_H_

#include <string>
#include <vector>

class JZWidget;

// 包装 Windows 原生文件对话框 (CommDlg32 / Shell), 对齐 Qt::QFileDialog 的常用静态接口。
//   - 内部直接调用 GetOpenFileNameW / GetSaveFileNameW / SHBrowseForFolderW;
//   - 字符串统一走 UTF-8 (std::string), 与框架一致;
//   - 取消 / 失败时返回空串 (或空 vector)。
//   - filter 形如 "Images (*.png *.jpg);;Text (*.txt)" (与 Qt 一致), 也可传空串 (= All Files)。
class JZFileDialog
{
public:
    // 选择单个已存在文件; 取消返回 ""。
    static std::string getOpenFileName(JZWidget* parent = nullptr,
                                       const std::string& caption = "",
                                       const std::string& dir = "",
                                       const std::string& filter = "");

    // 选择多个已存在文件; 取消返回空 vector。
    static std::vector<std::string> getOpenFileNames(JZWidget* parent = nullptr,
                                                     const std::string& caption = "",
                                                     const std::string& dir = "",
                                                     const std::string& filter = "");

    // 选择保存路径 (可不存在); 返回路径, 取消返回 ""。
    static std::string getSaveFileName(JZWidget* parent = nullptr,
                                       const std::string& caption = "",
                                       const std::string& dir = "",
                                       const std::string& filter = "");

    // 选择已存在目录; 返回目录路径, 取消返回 ""。
    static std::string getExistingDirectory(JZWidget* parent = nullptr,
                                            const std::string& caption = "",
                                            const std::string& dir = "");
};

#endif // !JZ_FILE_DIALOG_H_
