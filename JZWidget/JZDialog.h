#ifndef JZ_DIALOG_H_
#define JZ_DIALOG_H_

#include "JZWindow.h"
#include "JZSignal.h"

// 对话框 = 独立原生窗口 (继承 JZWindow, 自持渲染设备)。
//   - parent 仅作为内存所有权 (JZObject 父), 不进入其可视控件树;
//   - exec() 模态: 显示窗口 -> 屏蔽其它设备窗口输入 -> 嵌套事件循环
//     (JZEventLoop 驱动定时器, 渲染与消息泵照常) -> 关闭后恢复;
//   - accept()/reject() 结束 exec 并隐藏窗口; 原生 WM_CLOSE 走 reject。
//
//     JZMessageBox::question(&win, "Confirm", "Sure?")->exec();
class JZDialog : public JZWindow
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
    void paintEvent(JZPainterEvent* event) override;

    Result m_result;
};

#endif
