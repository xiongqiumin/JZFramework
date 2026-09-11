#ifndef JZ_JS_WIDGET_MODULE_H_
#define JZ_JS_WIDGET_MODULE_H_

#include <string>
#include <vector>
#include "JZJSEngine.h"
#include "JZColor.h"
#include "quickjs/quickjs.h"

class JZJSEngine;
class JZWindow;
class JZVBoxLayout;
class JZCanvas;
class JZPainter;

// C++ 包裹对象: 由 JS GC 拥有, 回收时析构。JS 侧的 JZWindow / JZCanvas 句柄
// 内部就是这两个结构; 引擎存活期间 context 始终有效, 析构时释放持有的 JS 回调引用。
struct JZJSWindow
{
    JZWindow* m_win = nullptr;
    JZVBoxLayout* m_box = nullptr;
    ~JZJSWindow();
};

struct JZJSCanvas
{
    JZJSEngine* m_engine = nullptr;
    JZCanvas* m_canvas = nullptr;
    JSValue m_paintCb = JS_UNDEFINED;
    JSValue m_mouseCb = JS_UNDEFINED;
    ~JZJSCanvas();
};

// 把 JZWidget 的窗口 / 画布暴露给 JS:
//   const win = JZWindow(w, h, title);          // 工厂函数, 返回窗口句柄
//   const cv  = win.createCanvas(w, h);          // 在窗口布局中创建画布
//   cv.setOnPaint((ctx) => { ... });             // ctx 走 JZPainter 后端
//   cv.setOnMouse((x, y, button) => { ... });    // 局部逻辑坐标
//   cv.update();                                  // 触发重绘
//   win.show();
// 测宽与绘制都复用框架现有链路, 窗口存在时 runtime 不退出。
class JZJSWidgetModule
{
public:
    JZJSWidgetModule();
    ~JZJSWidgetModule();

    void initEngine(JZJSEngine* engine);
    void deinitEngine();

private:
    JZJSEngine* m_engine = nullptr;
    int m_typeWindow = -1;
    int m_typeCanvas = -1;

    JSValue createWindow(int w, int h, const std::string& title);
    JSValue winCreateCanvas(JZJSWindow* win, int w, int h);
    void winSetTitle(JZJSWindow* win, const std::string& t);
    void winShow(JZJSWindow* win);
    void winClose(JZJSWindow* win);

    void cvSetOnPaint(JZJSCanvas* c, JSValue cb);
    void cvSetOnMouse(JZJSCanvas* c, JSValue cb);
    void cvUpdate(JZJSCanvas* c);
    JSValue cvGetSize(JZJSCanvas* c);

    void invokePaint(JZJSCanvas* c, JZPainter* p);
    void invokeMouse(JZJSCanvas* c, int x, int y, int button);

    static JZColor parseColor(const std::string& s);
};

#endif // !JZ_JS_WIDGET_MODULE_H_
