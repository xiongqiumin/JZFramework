#include "main_window.h"

MainWindow::MainWindow(JZWidget* parent)
    : JZWindow(parent)
{
    resize(800, 600);
    setTitle("JZWidget GDI - 飞行射击");

    m_game = new GameWidget(this);
    m_game->setGeometry(0, 0, 800, 600);
    m_game->show();
}

MainWindow::~MainWindow()
{
}

void MainWindow::resizeEvent(JZResizeEvent* event)
{
    JZWindow::resizeEvent(event);
    if (m_game)
        m_game->setGeometry(0, 0, width(), height());
}
