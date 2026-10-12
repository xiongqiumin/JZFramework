#include "main_window.h"

MainWindow::MainWindow(JZWidget* parent)
    : JZWindow(parent)
{
    resize(800, 600);
    setTitle("JZGame - shooter (JZGameView)");

    m_game = new ShooterGame(this);
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
