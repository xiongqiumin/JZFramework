#include "JZWindow.h"

JZ_OBJECT_IMPL(JZWindow, JZDisplayDevice)

JZWindow::JZWindow(int w, int h, const std::string& title)
    : JZDisplayDevice(w, h, title)
{
}

JZWindow::~JZWindow()
{
}
