#ifndef JZ_PAINTER_PATH_H_
#define JZ_PAINTER_PATH_H_
#include "JZMatrix2D.h"
#include "JZPoint.h"

#include <vector>

// 矢量路径: 记录 moveTo/lineTo/cubicTo/quadTo/closeSubpath 等图元,
// 供 JZPainter::drawPath (描边) / fillPath (填充) 使用 —— 等价 Qt QPainterPath
// 的 2D 子集 (曲线由三次/二次贝塞尔表示, 椭圆在 addEllipse 时即展开为 4 段三次贝塞尔,
// 因此路径只含 Move/Line/Cubic/Quad/Close 四种图元, 变换时无需特殊处理椭圆旋转)。
//
// 坐标约定: 记录的是**控件本地坐标**; 真正下发给后端前, JZPainter 会调用
// transformed() 把每个点过一遍当前变换矩阵并平移到显示坐标。
class JZFRAMEWORK_API JZPainterPath
{
public:
    enum ElementType {
        Move,    // d[0..1] = x,y (开启一条新子路径)
        Line,    // d[0..1] = x,y
        Cubic,   // d[0..5] = c1x,c1y,c2x,c2y,x,y
        Quad,    // d[0..5] = cx,cy,x,y
        Close    // (无参数) 闭合当前子路径
    };

    struct Element {
        ElementType type;
        double d[6];
    };

    void moveTo(double x, double y)
    {
        m_elems.push_back({Move, {x, y, 0, 0, 0, 0}});
    }

    void moveTo(const JZPoint& p) { moveTo(p.x(), p.y()); }

    void lineTo(double x, double y)
    {
        m_elems.push_back({Line, {x, y, 0, 0, 0, 0}});
    }

    void lineTo(const JZPoint& p) { lineTo(p.x(), p.y()); }

    // 三次贝塞尔: 从当前点到 (x,y), 控制点 (c1x,c1y),(c2x,c2y)。
    void cubicTo(double c1x, double c1y, double c2x, double c2y, double x, double y)
    {
        m_elems.push_back({Cubic, {c1x, c1y, c2x, c2y, x, y}});
    }

    // 二次贝塞尔: 从当前点到 (x,y), 控制点 (cx,cy)。
    void quadTo(double cx, double cy, double x, double y)
    {
        m_elems.push_back({Quad, {cx, cy, x, y, 0, 0}});
    }

    void closeSubpath()
    {
        m_elems.push_back({Close, {0, 0, 0, 0, 0, 0}});
    }

    void addRect(double x, double y, double w, double h)
    {
        moveTo(x, y);
        lineTo(x + w, y);
        lineTo(x + w, y + h);
        lineTo(x, y + h);
        closeSubpath();
    }

    void addEllipse(double x, double y, double w, double h)
    {
        double cx = x + w * 0.5;
        double cy = y + h * 0.5;
        double rx = w * 0.5;
        double ry = h * 0.5;
        const double k = 0.5522847498307936;   // 圆->4 段贝塞尔的魔法常数
        double kx = rx * k;
        double ky = ry * k;

        moveTo(cx + rx, cy);
        cubicTo(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
        cubicTo(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
        cubicTo(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
        cubicTo(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
        closeSubpath();
    }

    // 椭圆弧 (角度单位: 度, 0=3点钟, 顺时针递增, 与 drawArc 一致)。
    // 以折线逼近 (每 ~15° 一段), 平滑度足够; 若需精确弧线请用 addEllipse 或后端弧。
    void addArc(double x, double y, double w, double h,
                double startDeg, double endDeg)
    {
        double cx = x + w * 0.5;
        double cy = y + h * 0.5;
        double rx = w * 0.5;
        double ry = h * 0.5;
        double sweep = endDeg - startDeg;
        int n = (int)(std::fabs(sweep) / 15.0) + 1;
        if (n < 1) n = 1;
        double step = sweep / n;
        double a = startDeg * 3.14159265358979323846 / 180.0;
        double px = cx + rx * std::cos(a);
        double py = cy + ry * std::sin(a);
        moveTo(px, py);
        for (int i = 1; i <= n; ++i) {
            a = (startDeg + step * i) * 3.14159265358979323846 / 180.0;
            px = cx + rx * std::cos(a);
            py = cy + ry * std::sin(a);
            lineTo(px, py);
        }
    }

    const std::vector<Element>& elements() const { return m_elems; }
    bool isEmpty() const { return m_elems.empty(); }

    // 把路径所有点过一遍矩阵 (本地->显示) 并平移 (ox,oy), 返回新路径。
    // 椭圆/弧在 addXxx 时已是贝塞尔/折线, 这里只搬点, 旋转也能正确保留。
    JZPainterPath transformed(const JZMatrix2D& m, double ox, double oy) const
    {
        JZPainterPath out;
        for (const Element& e : m_elems) {
            switch (e.type) {
            case Move:  case Line: {
                double nx, ny; m.map(e.d[0], e.d[1], nx, ny);
                out.m_elems.push_back({e.type, {nx + ox, ny + oy, 0, 0, 0, 0}});
                break;
            }
            case Cubic: {
                double c1x, c1y, c2x, c2y, ex, ey;
                m.map(e.d[0], e.d[1], c1x, c1y);
                m.map(e.d[2], e.d[3], c2x, c2y);
                m.map(e.d[4], e.d[5], ex, ey);
                out.m_elems.push_back({Cubic, {c1x + ox, c1y + oy, c2x + ox, c2y + oy, ex + ox, ey + oy}});
                break;
            }
            case Quad: {
                double cx, cy, ex, ey;
                m.map(e.d[0], e.d[1], cx, cy);
                m.map(e.d[2], e.d[3], ex, ey);
                out.m_elems.push_back({Quad, {cx + ox, cy + oy, ex + ox, ey + oy, 0, 0}});
                break;
            }
            case Close:
                out.m_elems.push_back({Close, {0, 0, 0, 0, 0, 0}});
                break;
            }
        }
        return out;
    }

private:
    std::vector<Element> m_elems;
};

#endif // JZ_PAINTER_PATH_H_
