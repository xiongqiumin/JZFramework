#pragma once

#include "JZWindow.h"
#include "game_widget.h"

// 游戏窗口: 作为 JZWindow 根, 内嵌一个铺满的 GameWidget。
class MainWindow : public JZWindow
{
public:
    explicit MainWindow(JZWidget* parent = nullptr);
    ~MainWindow();

protected:
    void resizeEvent(JZResizeEvent* event) override;

private:
    GameWidget* m_game = nullptr;
};
