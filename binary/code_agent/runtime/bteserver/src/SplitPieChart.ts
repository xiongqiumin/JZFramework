// ---------------------------------------------------------------------------
// SplitPieChart.js —— 对应 C++ BTEServer/SplitPieChart.h + SplitPieChart.cpp
//
// 分选结果饼图: 内部用第三方 Nightcharts 画饼 + 竖排图例。
//
// 说明: nightcharts(与 qcustomplot / sqlite3 / libxlsx 一样)是随工程分发的
//       第三方 Qt 类, 不参与 JS 移植, 此处保留调用点, 由运行时提供等价实现。
// ---------------------------------------------------------------------------

import { tr } from './BTEServer.js';
import { QBrush, QColor, QLinearGradient, QRect, QTextOption, Qt } from './qtcompat.js';
import { qMin } from './BTEProject.js';

// ---------------------------------------------------------------------------
// class SplitPieChart : public QWidget
// ---------------------------------------------------------------------------
export class SplitPieChart
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // explicit SplitPieChart(QWidget *parent = 0);
    constructor(parent)
    {
        QWidget.call(this, parent);   // : QWidget(parent)

        this._chart = new Nightcharts();   // Nightcharts _chart;
        this.clear();
    }

    // void clear();
    clear()
    {
        this._chart = new Nightcharts();
        this._chart.setType(Nightcharts.Pie);
        this._chart.setLegendType(Nightcharts.Vertical);
    }

    // void setResult(SplitResult result);
    setResult(result)
    {
        this.clear();

        const colorList = [];
        colorList.push(Qt.red, Qt.green, Qt.blue, Qt.yellow, Qt.cyan, Qt.magenta,
            Qt.white, Qt.gray, Qt.darkRed, Qt.darkGreen, Qt.darkBlue,
            Qt.lightGray, Qt.darkYellow, Qt.darkCyan, Qt.darkMagenta, Qt.darkGray);

        let color_idx = 0;
        for (let i = 0; i < result.indexs.length; i++)
        {
            if (result.indexs[i].length !== 0)
            {
                const name = (i < 15) ? String.fromCharCode('A'.charCodeAt(0) + i) : tr("落选");
                this._chart.addPiece(name, colorList[color_idx++], result.indexs[i].length / result.sumItem * 100);
            }
        }
        this.update();
    }

    // virtual void paintEvent(QPaintEvent * e);
    paintEvent(e)
    {
        const back_gradient = new QLinearGradient(0, 0, this.width(), this.height());
        back_gradient.setColorAt(0, new QColor(240, 240, 240));
        back_gradient.setColorAt(1, new QColor(60, 60, 60));

        const back = new QBrush(back_gradient);

        const painter = new QPainter(this);
        painter.fillRect(this.rect(), back);

        const text_rc = new QRect(0, 20, this.width(), 60);
        const font = painter.font();
        font.setPixelSize(40);
        painter.setFont(font);

        painter.drawText(text_rc, tr("分选结果"), new QTextOption(Qt.AlignCenter));
        font.setPixelSize(16);
        painter.setFont(font);

        const margin_left = 10;
        const margin_top = 90;
        const legend_width = 120;
        if (this._chart.pieceCount() > 0)
        {
            const w = this.width() - 2 * margin_left - legend_width;
            const h = this.height() - margin_top - 10;
            const size = qMin(w, h) / 1.2;
            this._chart.setCords(margin_left + ((w - size) / 2) | 0, margin_top, size, size);

            this._chart.draw(painter);

            this._chart.setLegendCords(margin_left + w, margin_top + 10);
            this._chart.drawLegend(painter);
        }
    }

    // void addItem(QString name, QColor color, float value);
    addItem(name, color, value)
    {
        this._chart.addPiece(name, color, value);
    }
}
