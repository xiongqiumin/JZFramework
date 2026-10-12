#pragma once

#include "JZWindow.h"
#include "danmaku_widget.h"

// 游戏窗口: 作为 JZWindow 根, 内嵌一个铺满的 DanmakuGame。
class MainWindow : public JZWindow
{
public:
    explicit MainWindow(JZWidget* parent = nullptr);
    ~MainWindow();

protected:
    void resizeEvent(JZResizeEvent* event) override;

private:
    DanmakuGame* m_game = nullptr;
};
