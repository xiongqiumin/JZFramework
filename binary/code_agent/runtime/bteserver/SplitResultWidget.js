// ---------------------------------------------------------------------------
// SplitResultWidget.js —— 对应 C++ BTEServer/SplitResultWidget.h + SplitResultWidget.cpp
//
// 分选结果点灯图: 4x8 小方块(蓝底绿块), 被点亮的通道画红色。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// class SplitResultWidget : public QWidget
// ---------------------------------------------------------------------------
export class SplitResultWidget
{
    // SplitResultWidget(QWidget *parent = Q_NULLPTR);
    constructor(parent)
    {
        QWidget.call(this, parent);   // : QWidget(parent)

        this.m_channels = [];
    }

    // ~SplitResultWidget();
    dispose()
    {
    }

    // virtual void paintEvent(QPaintEvent *event) override;
    paintEvent(event)
    {
        const painter = new QPainter(this);
        const x_offset = 12;
        const y_offset = 12;
        const space = 8;

        const backColor = qRgb(21, 29, 202);
        const barColor = qRgb(73, 196, 165);
        const redColor = qRgb(255, 0, 0);
        const nameColor = qRgb(173, 232, 148);
        painter.fillRect(this.rect(), backColor);

        const w = ((this.rect().width() - 2 * x_offset - space * 7) / 8) | 0;
        const h = ((this.rect().height() - 2 * y_offset - space * 3) / 4) | 0;

        const x_gap = (this.rect().width() - 2 * x_offset - w * 8) / 7.0;
        const y_gap = (this.rect().height() - 2 * y_offset - h * 4) / 3.0;

        const rc = new QRect(0, 0, w, h);
        for (let i = 0; i < 4; i++)
        {
            for (let j = 0; j < 8; j++)
            {
                rc.moveTo(x_offset + (w + x_gap) * j, y_offset + (h + y_gap) * i);

                const item_idx = i * 8 + j;
                if (this.m_channels.indexOf(item_idx) >= 0)
                    painter.fillRect(rc, redColor);
                else
                    painter.fillRect(rc, barColor);
            }
        }
    }

    // void setChannels(QVector<int> channels);   //设置点亮的通道
    setChannels(channels)
    {
        this.m_channels = channels;
        this.update();
    }
}
