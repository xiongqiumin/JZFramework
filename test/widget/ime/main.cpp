// IME 候选框落点验证用最小对话框。
//
// 用法:
//   1) 编译生成 test_ime.exe (见本目录 CMakeLists.txt)。
//   2) 运行, 程序会尝试把键盘布局切到中文(中国), 并把文本框设为默认焦点。
//   3) 若没自动切到中文输入法, 手动 "切换过去" (Win+Space / Ctrl+Shift) 即可。
//   4) 在文本框里打字, 观察候选框是否落在输入字符正下方。
//   5) 需要诊断坐标时, 在运行前设环境变量 JZ_IME_DEBUG=1, 跑完看 %TEMP%/jz_ime.log。
#include "JZApplication.h"
#include "JZWindow.h"
#include "JZLabel.h"
#include "JZTextEdit.h"
#include "JZLineEdit.h"
#include "JZLayout.h"

#ifdef _WIN32
#  include <windows.h>
#endif

int app_main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

#ifdef _WIN32
    // 尽力把键盘布局切到中文(中国), 让程序"默认中文" (IME 是否真正上屏仍取决于系统设置)。
    // 失败也无妨, 用户可手动切换。
    HKL hkl = LoadKeyboardLayoutW(L"00000804", KLF_ACTIVATE | KLF_SUBSTITUTE_OK);
    if (hkl)
        ActivateKeyboardLayout(hkl, 0);
#endif

    JZApplication app;

    JZWindow window;
    window.resize(640, 480);
    window.setTitle("IME Test (中文输入候选框落点验证)");

    JZVBoxLayout* layout = new JZVBoxLayout(10, 10);

    JZLabel* hint = new JZLabel(&window);
    hint->setText("请切换到中文输入法, 点击下方输入框后打字,\n"
                  "观察候选框是否落在输入字符正下方。");
    layout->addWidget(hint, 0);

    JZTextEdit* te = new JZTextEdit(&window);
    te->setPlaceholderText("多行文本框: 在这里输入中文");
    te->setMinimumHeight(200);
    layout->addWidget(te, 1);

    JZLineEdit* le = new JZLineEdit(&window);
    le->setPlaceholderText("单行输入框: 在这里输入中文");
    layout->addWidget(le, 0);

    window.setLayout(layout);

    // 默认焦点给多行文本框, 启动即可直接打字。
    window.setFocusWidget(te);
    window.show();

    return app.exec();
}

int main(int argc, char* argv[])
{
    return app_main(argc, argv);
}
