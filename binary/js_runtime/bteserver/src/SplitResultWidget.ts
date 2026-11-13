// ---------------------------------------------------------------------------
// SplitResultWidget.ts —— 对应 C++ BTEServer/SplitResultWidget.h + SplitResultWidget.cpp
//
// 分选结果点灯图: 4x8 小方块(蓝底绿块), 被点亮的通道画红色。
//
// 迁移(2026-09-14): 原来是 `QWidget.call(this, parent)` + 在 paintEvent 里用 qtcompat 的
// QPainter/QRect/qRgb 画。现在**本身就是真控件**(extends JZWidget), 自绘走引擎的 onPaint:
//   * 回调签名 onPaint(painter), painter 是引擎为本次派发建的 JZPainter(生命周期=本次派发);
//   * 画笔接口是"坐标 + CSS 颜色串"的形态: fillRect(x, y, w, h, '#rrggbb');
//   * 控件尺寸用 this.width() / this.height()(真控件接口), 不再有 QRect/rect()。
// ---------------------------------------------------------------------------

// 颜色沿用原 C++ 的 qRgb 值: back(21,29,202) bar(73,196,165) red(255,0,0)
const COLOR_BACK = '#151dca';
const COLOR_BAR = '#49c4a5';
const COLOR_RED = '#ff0000';

// ---------------------------------------------------------------------------
// class SplitResultWidget : public QWidget
// ---------------------------------------------------------------------------
// 迁移: 组合, 不继承 —— 引擎的控件构造器返回自己的对象, `extends JZWidget` 会丢子类方法
//       (实测 instanceof 为 false / constructor.name === "JZWidget"), 所以持一个真控件 this.root。
export class SplitResultWidget
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明

    // SplitResultWidget(QWidget *parent = Q_NULLPTR);
    constructor(parent)
    {
        this.root = new JZWidget(parent);   // : QWidget(parent)

        this.m_channels = [];

        // 自绘入口: 引擎的 onPaint 属性(回调存在模块侧, 与包装对象解耦)
        this.root.onPaint = (painter) => this.paintEvent(painter);
    }

    // virtual void paintEvent(QPaintEvent *event) override;
    paintEvent(painter)
    {
        const x_offset = 12;
        const y_offset = 12;
        const space = 8;

        const w_all = this.root.width();
        const h_all = this.root.height();

        painter.fillRect(0, 0, w_all, h_all, COLOR_BACK);

        const w = ((w_all - 2 * x_offset - space * 7) / 8) | 0;
        const h = ((h_all - 2 * y_offset - space * 3) / 4) | 0;

        const x_gap = (w_all - 2 * x_offset - w * 8) / 7.0;
        const y_gap = (h_all - 2 * y_offset - h * 4) / 3.0;

        for (let i = 0; i < 4; i++)
        {
            for (let j = 0; j < 8; j++)
            {
                const x = (x_offset + (w + x_gap) * j) | 0;
                const y = (y_offset + (h + y_gap) * i) | 0;

                const item_idx = i * 8 + j;
                // 原 C++: 点亮(在 m_channels 里)的画红色, 其余画绿块
                painter.fillRect(x, y, w, h,
                    (this.m_channels.indexOf(item_idx) >= 0) ? COLOR_RED : COLOR_BAR);
            }
        }
    }

    // void setChannels(QVector<int> channels);   //设置点亮的通道
    setChannels(channels)
    {
        this.m_channels = channels;
        this.root.update();   // 触发重绘(等价 QWidget::update())
    }
}
