// 拖放示例: 往窗口里拖图片 / 文件 / 文本。
//   * 上面的大方块是图片区: 从资源管理器拖一张 png/jpg 进来, 或者从浏览器里
//     直接把图片拖进来, 它都会接住并显示; 拖着经过时描黄色高亮边框, 拖走复原。
//   * 下面的文本区接文本 / 任意文件: 显示路径清单与前 200 字。
// 顺带演示 Qt 语义的两个点: 只有 setAcceptDrops 的控件参与; 事件不接就沿父链上抛
// (这里两个区各自接管, 窗口本身没开, 所以拖到空白区域没有任何控件接)。

#include <cstdio>
#include <string>
#include <vector>

#include "JZApplication.h"
#include "JZWindow.h"
#include "JZWidget.h"
#include "JZLabel.h"
#include "JZLayout.h"
#include "JZPainter.h"
#include "JZImage.h"
#include "JZResizeEvent.h"
#include "JZDragEvent.h"

// 图片落点: 自绘一张按宽高比居中的图, 拖拽经过时描高亮边框
class ImageDrop : public JZWidget
{
    JZ_OBJECT_DECLARE(ImageDrop)

public:
    explicit ImageDrop(JZWidget* parent = nullptr)
        : JZWidget(parent)
    {
        setAcceptDrops(true);
    }

    JZSize sizeHint() const override { return JZSize(480, 320); }

protected:
    void dragEnterEvent(JZDragEnterEvent* event) override
    {
        // 只接"有图"的那类数据: 内联位图, 或者拖进来的第一个文件是图片。
        // 不表态 (不 accept) = 不接, 事件继续往父控件抛。
        if (!event->mimeData()->hasImage())
            return;

        m_hover = true;
        update();
        event->acceptProposedAction();   // 接下: 拖方据此显示"可放下"的光标
    }

    void dragMoveEvent(JZDragMoveEvent* event) override
    {
        event->acceptProposedAction();   // 每次移动都要重新表态
    }

    void dragLeaveEvent(JZDragLeaveEvent* event) override
    {
        m_hover = false;
        update();
    }

    void dropEvent(JZDropEvent* event) override
    {
        m_hover = false;
        JZImage img = event->mimeData()->image();   // 内联位图, 或第一个图片文件解码
        if (img.isNull())
            return;                                 // 解不出来就当没发生

        m_image = img;
        update();
        event->acceptProposedAction();
        if (event->dropAction() == JZDropActionMove)
            std::printf("[image] drop=Move (源可以据此删掉自己那份数据)\n");
    }

    void paintEvent(JZPainterEvent* event) override
    {
        JZPainter p(this);
        p.fillRect(0, 0, width(), height(), JZColor(246, 246, 246));

        const int pad = 8;
        int bw = width() - pad * 2;
        int bh = height() - pad * 2;
        if (!m_image.isNull() && bw > 0 && bh > 0 && m_image.width() > 0) {
            int w = bw, h = bh;
            if (m_image.width() * h > w * m_image.height())
                h = m_image.height() * w / m_image.width();
            else
                w = m_image.width() * h / m_image.height();
            if (w > 0 && h > 0) {
                int x = pad + (bw - w) / 2;
                int y = pad + (bh - h) / 2;
                p.drawImage(m_image, x, y, w, h);
            }
        }

        p.setPen(JZPen(m_hover ? JZColor(255, 170, 0) : JZColor(200, 200, 200), m_hover ? 2 : 1));
        p.drawRect(1, 1, width() - 2, height() - 2);
    }

private:
    JZImage m_image;
    bool    m_hover = false;
};

JZ_OBJECT_IMPL(ImageDrop, JZWidget)

// 文本落点: 把接到的东西逐条列出来
class TextDrop : public JZWidget
{
    JZ_OBJECT_DECLARE(TextDrop)

public:
    explicit TextDrop(JZWidget* parent = nullptr)
        : JZWidget(parent)
    {
        setAcceptDrops(true);
    }

    JZSize sizeHint() const override { return JZSize(480, 140); }

protected:
    void dragEnterEvent(JZDragEnterEvent* event) override
    {
        if (!event->mimeData()->hasText() && !event->mimeData()->hasUrls())
            return;
        event->acceptProposedAction();
    }

    void dropEvent(JZDropEvent* event) override
    {
        JZMimeData* data = event->mimeData();
        m_lines.clear();

        std::vector<std::string> urls = data->urls();
        for (size_t i = 0; i < urls.size(); ++i)
            m_lines.push_back("file: " + urls[i]);
        if (data->hasText()) {
            std::string text = data->text();
            m_lines.push_back("text(" + std::to_string(text.size()) + "): " +
                              text.substr(0, text.size() > 200 ? 200 : text.size()));
        }
        if (m_lines.empty())
            m_lines.push_back("(没有能识别的内容, formats=" +
                              std::to_string(data->formats().size()) + ")");
        update();
        event->acceptProposedAction();
    }

    void paintEvent(JZPainterEvent* event) override
    {
        JZPainter p(this);
        p.fillRect(0, 0, width(), height(), JZColor(255, 255, 255));

        p.setPen(JZPen(JZColor(200, 200, 200), 1));
        p.drawRect(1, 1, width() - 2, height() - 2);

        p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
        int y = 8;
        for (size_t i = 0; i < m_lines.size() && y < height() - 8; ++i) {
            p.drawText(10, y, m_lines[i].c_str());
            y += 18;
        }
    }

private:
    std::vector<std::string> m_lines;
};

JZ_OBJECT_IMPL(TextDrop, JZWidget)

class MainWin : public JZWindow
{
    JZ_OBJECT_DECLARE(MainWin)

public:
    MainWin()
        : JZWindow(520, 540, "JZWidget drag & drop")
    {
        m_content = new JZWidget(this);

        JZVBoxLayout* layout = new JZVBoxLayout(8, 8);
        layout->setContentsMargins(16, 16, 16, 16);

        JZLabel* tip = new JZLabel("拖图片到上面 (图片文件 or 浏览器里的图), 拖文本/文件到下面");
        tip->setFixedHeight(24);
        layout->addWidget(tip);
        layout->addWidget(new ImageDrop(), 1);
        layout->addWidget(new TextDrop());

        m_content->setLayout(layout);
        m_content->move(0, 0);
        m_content->resize(width(), height());
        m_content->show();
    }

protected:
    void resizeEvent(JZResizeEvent* event) override
    {
        JZWindow::resizeEvent(event);
        if (m_content)
            m_content->resize(width(), height());
    }

private:
    JZWidget* m_content = nullptr;
};

JZ_OBJECT_IMPL(MainWin, JZWindow)

int app_main(int argc, char* argv[])
{
    (void)argc; (void)argv;

    JZApplication app;
    MainWin win;
    win.show();
    return app.exec();
}

int main(int argc, char* argv[])
{
    return app_main(argc, argv);
}
