#ifndef JZ_DIALOG_H_
#define JZ_DIALOG_H_
#include "JZWindow.h"
#include "JZSignal.h"

// 对话框 = 独立原生窗口 (继承 JZWindow, 自持渲染设备)。
//   - parent 仅作为内存所有权 (JZObject 父), 不进入其可视控件树;
//   - exec() 模态 (DialogBoxParam 语义): 对话框挂 owner (父链所属窗口,
//     JZWindow::createWindow 传 hWndParent), exec 禁用 owner -> 自管嵌套循环
//     (参考 JZWin32Thread::run: 泵 Win32 消息 + 框架事件/定时器 + 整窗直绘) -> 退出恢复;
//   - accept()/reject() 结束 exec 并隐藏窗口; 原生 WM_CLOSE 走 reject。
//
//     JZMessageBox::question(&win, "Confirm", "Sure?")->exec();
class JZFRAMEWORK_API JZDialog : public JZWindow
{
    JZ_OBJECT_DECLARE(JZDialog)

public:
    enum Result { Rejected = 0, Accepted = 1 };

    JZDialog(JZWidget* parent = nullptr);

    Result result() const;

    int exec();
    void accept();
    void reject();

jz_signals:
    JZSigVoid sigAccepted;
    JZSigVoid sigRejected;
    JZSigInt  sigFinished;  // Result

protected:
    void closeEvent(JZCloseEvent* event) override;
    void keyPressEvent(JZKeyEvent* event) override;
    void paintEvent(JZPainterEvent* event) override;

    Result m_result;
    // exec() 正在跑(模态循环进行中): 对齐 Qt —— 再对同一对象调 exec() 时打警告并返回 -1,
    // 不动 m_result(见 JZDialog.cpp)。不同对话框互相嵌套是支持的(独立栈帧)。
    bool m_inExec = false;
};

#endif
