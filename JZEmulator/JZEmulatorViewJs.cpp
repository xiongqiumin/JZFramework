// ⚠ windows.h 要在最前（有保护宏）：框架头会带进 windows.h
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif

#include "JZEmulatorViewJs.h"

#include <string>
#include <vector>

#include "JZEmulatorView.h"
#include "JZJSEngine.h"
#include "JZJSBind.h"
#include "JZJSWidgetModule.h"

namespace
{
    // JS 侧 `new JZEmulatorView(parent)` —— parent 可省 / 可 null（与其它控件同一套形态：
    // 实参原样转发到 C 侧形态分派，null/undefined/缺省都表示"无父"）。
    void* js_JZEmulatorView_ctor(JZJSEngine* engine, int argc, JSValueConst* argv)
    {
        std::vector<ArgResult> r;
        if (engine->checkAndSetArgs("JZWidget=null", &r) == -1)
            return nullptr;             // 形参不符：异常已由 checkAndSetArgs 抛出
        JZWidget* parent = static_cast<JZWidget*>(r[0].p);
        return new JZEmulatorView(parent);
    }
}

void jzRegisterEmulatorView(JZJSEngine* engine)
{
    if (!engine)
        return;
    // 没装 widget 模块（没启用 JZWidget）时没什么可挂的
    JZJSWidgetModule* mod = engine->widgetModule();
    if (!mod)
        return;

    // ------------------------------------------------------------------
    // ① 先在 JS 里定义这个类（**不放进框架的 widget 包**）：
    //   widget/classes.ts 是对外的公共类层，本控件属于"装了模块才有"的内部能力，
    //   所以类定义就在这里 eval 一段 —— 好处是零改动 js_wrapper：不用 tsc、不用
    //   package.py，也就没有"改了 .ts 忘了重编译 dist"那个坑（见 JZEmulator.cmake）。
    //
    // 形状必须与 classes.ts 的 defWidgetClass 等价，否则实例拿不到 op 槽 / 继承不到基类方法：
    //   · 实例必须由全局 __JZWidgetObject 造出来 —— quickjs 的 opaque 槽只对自定义 class 生效，
    //     普通 `class X {}` new 出来的对象挂不上；所以 extends __JZWidgetObject + super()；
    //   · 紧接着 init_c_emulator_view(this, ...args) 造真正那份底层对象
    //     （这一步不能省，也不能靠 JZWidget 的构造器代劳：那会凭空多造一个 JZWidget）；
    //   · 最后把原型链接到 JZWidget.prototype 上，setGeometry/show/setFocus 这些基类方法
    //     才找得到（classes.ts 里是直接 extends JZWidget 做到的，但那条路会走它的构造器）。
    // 幂等：重复注册不重建（重建会换掉原型，先前挂上去的方法就没了）。
    // ------------------------------------------------------------------
    engine->eval(R"JS(
(function () {
    if (typeof globalThis.JZEmulatorView === 'function')
        return;
    if (typeof __JZWidgetObject !== 'function')
        throw new Error('JZEmulatorView: 引擎没装 widget 模块(缺 __JZWidgetObject)');
    class JZEmulatorView extends __JZWidgetObject {
        constructor(...args) {
            super();
            init_c_emulator_view(this, ...args);
        }
    }
    Object.setPrototypeOf(JZEmulatorView.prototype, JZWidget.prototype);
    globalThis.JZEmulatorView = JZEmulatorView;
})();
)JS");

    // ② 再把方法挂到这个类的原型上。⚠ 形参一律写 JZObjectPointer*（控件族没进引擎类型表），
    //   函数体里 dynamic_cast 回具体类；写成 JZEmulatorView* 会 static_cast 出野指针
    //   —— 见 JZJSWidgetModule.h 的 widgetFromThis 说明。
    jsbind::ClassBind<JZEmulatorView> bind("JZEmulatorView");

    bind.def("openRom", [mod](JZObjectPointer* w, const std::string& dllDir, const std::string& romPath) {
        JZEmulatorView* v = mod->widgetFromThis<JZEmulatorView>(w, "openRom");
        return v ? v->openRom(dllDir, romPath) : false;
    });
    bind.def("isRomOpen", [mod](JZObjectPointer* w) {
        JZEmulatorView* v = mod->widgetFromThis<JZEmulatorView>(w, "isRomOpen");
        return v ? v->isRomOpen() : false;
    });
    bind.def("lastError", [mod](JZObjectPointer* w) {
        JZEmulatorView* v = mod->widgetFromThis<JZEmulatorView>(w, "lastError");
        return v ? v->lastError() : std::string();
    });
    bind.def("start", [mod](JZObjectPointer* w) {
        if (JZEmulatorView* v = mod->widgetFromThis<JZEmulatorView>(w, "start"))
            v->start();
    });
    bind.def("stop", [mod](JZObjectPointer* w) {
        if (JZEmulatorView* v = mod->widgetFromThis<JZEmulatorView>(w, "stop"))
            v->stop();
    });
    bind.def("isRunning", [mod](JZObjectPointer* w) {
        JZEmulatorView* v = mod->widgetFromThis<JZEmulatorView>(w, "isRunning");
        return v ? v->isRunning() : false;
    });
    bind.def("frameCount", [mod](JZObjectPointer* w) {
        JZEmulatorView* v = mod->widgetFromThis<JZEmulatorView>(w, "frameCount");
        return v ? v->frameCount() : 0;
    });

    bind.setCreate(js_JZEmulatorView_ctor);

    // 真正挂上去：把上面这些方法装到 classes.ts 里那个 JZEmulatorView 原型上，
    // 并把 js_JZEmulatorView_ctor 注册成全局 init_c_emulator_view(this, ...)。
    mod->mountTransparentClass(bind.classDefine());
}
