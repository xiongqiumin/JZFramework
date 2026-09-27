#ifndef JZ_MATRIX2D_H_
#define JZ_MATRIX2D_H_
#include "JZGlobal.h"
#include <cmath>
#include <cstddef>

// 2D 仿射变换矩阵, 用**标准 3x3 矩阵**表示 (行主序):
//
//   | m00  m01  m02 |       x' = m00*x + m01*y + m02
//   | m10  m11  m12 |  =>   y' = m10*x + m11*y + m12
//   |  0    0    1  |       1  = 1
//
// 由于仿射变换末行恒为 [0, 0, 1], 计算时仅维护前两行, 末行保持不变量。
//
// 坐标系: 与窗口/屏幕一致 y 向下。rotate(正角度) 在屏幕上表现为**顺时针**
// (与 JZPainter::drawArc 的角度约定一致: 0=3点钟, 顺时针递增)。
//
// 组合语义 (post-multiply, 同 Qt): 每次 translate/rotate/scale 都右乘到当前矩阵,
// 即 "在已经变换过的坐标系内" 继续变换。例如先 translate 把原点搬到控件中心,
// 再 rotate, 则绕控件中心旋转。
class JZFRAMEWORK_API JZMatrix2D
{
public:
    JZMatrix2D()
    {
        reset();
    }

    // 置为单位阵 I。
    void reset()
    {
        m[0][0] = 1; m[0][1] = 0; m[0][2] = 0;
        m[1][0] = 0; m[1][1] = 1; m[1][2] = 0;
        m[2][0] = 0; m[2][1] = 0; m[2][2] = 1;
    }

    bool isIdentity() const
    {
        return m[0][0] == 1 && m[0][1] == 0 && m[0][2] == 0 &&
               m[1][0] == 0 && m[1][1] == 1 && m[1][2] == 0;
    }

    // 元素读写 (标准 3x3 下标访问, 末行 [2][0..2] 通常保持 0/0/1)。
    double at(int row, int col) const { return m[row][col]; }
    double& at(int row, int col)      { return m[row][col]; }

    // 设置 2x3 仿射部分 (行主序, 末行自动置 [0,0,1])。
    void setMatrix(double m00, double m01, double m02,
                   double m10, double m11, double m12)
    {
        m[0][0] = m00; m[0][1] = m01; m[0][2] = m02;
        m[1][0] = m10; m[1][1] = m11; m[1][2] = m12;
        m[2][0] = 0;   m[2][1] = 0;   m[2][2] = 1;
    }

    // 设置完整 3x3 (含末行, 一般仍为 [0,0,1])。
    void setMatrix3(double m00, double m01, double m02,
                    double m10, double m11, double m12,
                    double m20, double m21, double m22)
    {
        m[0][0] = m00; m[0][1] = m01; m[0][2] = m02;
        m[1][0] = m10; m[1][1] = m11; m[1][2] = m12;
        m[2][0] = m20; m[2][1] = m21; m[2][2] = m22;
    }

    // 当前矩阵在 x / y 方向上的缩放量 (供按图元尺寸估算用)。
    // 等于 x 轴单位向量 (1,0) / y 轴单位向量 (0,1) 经变换后的长度。
    double scaleX() const { return std::sqrt(m[0][0] * m[0][0] + m[1][0] * m[1][0]); }
    double scaleY() const { return std::sqrt(m[0][1] * m[0][1] + m[1][1] * m[1][1]); }

    void translate(double dx, double dy)
    {
        multiply(JZMatrix2D::translation(dx, dy));
    }

    void scale(double sx, double sy)
    {
        multiply(JZMatrix2D::scaling(sx, sy));
    }

    void rotate(double deg)
    {
        multiply(JZMatrix2D::rotation(deg));
    }

    void rotateRadians(double rad)
    {
        multiply(JZMatrix2D::rotationRadians(rad));
    }

    // this = this * o  (先应用 o, 再应用 this)
    void multiply(const JZMatrix2D& o)
    {
        double r[3][3];
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                r[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j];
            }
        }
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                m[i][j] = r[i][j];
    }

    JZMatrix2D multiplied(const JZMatrix2D& o) const
    {
        JZMatrix2D r = *this;
        r.multiply(o);
        return r;
    }

    // 仿射逆矩阵 (末行恒 [0,0,1] 时行列式 det = a*e - b*d)。det 接近 0 时退化为单位阵。
    JZMatrix2D inverse() const
    {
        double a = m[0][0], b = m[0][1], c = m[0][2];
        double d = m[1][0], e = m[1][1], f = m[1][2];
        double det = a * e - b * d;
        if (std::fabs(det) < 1e-12)
            return JZMatrix2D();   // 退化, 返回 I
        double id = 1.0 / det;
        JZMatrix2D r;
        r.m[0][0] =  e * id;  r.m[0][1] = -b * id;  r.m[0][2] = (b * f - e * c) * id;
        r.m[1][0] = -d * id;  r.m[1][1] =  a * id;  r.m[1][2] = (d * c - a * f) * id;
        r.m[2][0] = 0;        r.m[2][1] = 0;         r.m[2][2] = 1;
        return r;
    }

    void map(double x, double y, double& ox, double& oy) const
    {
        ox = m[0][0] * x + m[0][1] * y + m[0][2];
        oy = m[1][0] * x + m[1][1] * y + m[1][2];
    }

    // 仅平移/缩放估算 (忽略旋转/错切): 把 (w,h) 各方向缩放后返回。
    void mapSize(double w, double h, double& ow, double& oh) const
    {
        ow = w * scaleX();
        oh = h * scaleY();
    }

    static JZMatrix2D translation(double dx, double dy)
    {
        JZMatrix2D m;
        m.m[0][2] = dx;
        m.m[1][2] = dy;
        return m;
    }

    static JZMatrix2D scaling(double sx, double sy)
    {
        JZMatrix2D m;
        m.m[0][0] = sx;
        m.m[1][1] = sy;
        return m;
    }

    // 屏幕坐标系 (y 向下): 正角度顺时针。标准 CCW 矩阵在 y 向下时视觉即顺时针。
    static JZMatrix2D rotation(double deg)
    {
        return rotationRadians(deg * 3.14159265358979323846 / 180.0);
    }

    static JZMatrix2D rotationRadians(double a)
    {
        double c = std::cos(a), s = std::sin(a);
        JZMatrix2D m;
        m.m[0][0] = c;  m.m[0][1] = -s;
        m.m[1][0] = s;  m.m[1][1] = c;
        return m;
    }

private:
    double m[3][3];
};

#endif // JZ_MATRIX2D_H_
