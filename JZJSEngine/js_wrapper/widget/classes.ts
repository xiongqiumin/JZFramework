// @jz/widget —— 控件族的**类层**(由引擎在装配 widget 模块时加载, 定义全局类)。
//
// 为什么类定义在这里, 而不是 C++ 侧 registClass:
//   控件族对 JZJSEngine **完全透明** —— 引擎不注册它们、没有 ctypeid/类型表, `checkType` /
//   `isInstanceOf` / 形参表都不认控件对象。类型契约由 types/*.d.ts 负责(TS 编译期),
//   运行期只有 op 槽(守卫)与各方法的兜底校验。
//
// C++ 只提供三样东西:
//   ① `__JZWidgetObject` —— 基构造器: 造出"带 op 槽"的实例(quickjs 的 JS_SetOpaque 只对自定义
//      class 生效, 所以实例必须由它造, `class X {}` 直接 new 出来的普通对象挂不上 opaque);
//   ② `init_c_xxx(this, ...args)` —— 造底层 C++ 对象并 set_op(this, 对象);
//   ③ 各原型方法(`c_xxx_yyy`, 原生 quickjs 函数)由 C++ 挂到下面这些原型上。
//
// 形状约定(必须与 C++ 侧一致):
//   * 类的继承关系与 C++ 一致(见 types/*.d.ts 的声明), 最上面 extends __JZWidgetObject;
//   * **只有基类**造自己的底层对象; 派生类一律 `super(NOCREATE)` 跳过祖先的构造, 再
//     `init_c_xxx(this, ...args)` 造真正那一份 —— 参数**原样转发**, 由 C 侧按实参形态分派到
//     对应的 C++ 构造重载(与 js_JZxxx_ctor 的形态表一一对应);
//   * 方法不在这里写(由 C++ 挂); 信号就是实例上的普通属性(`btn.onClicked = fn`), C++ 侧在对象
//     诞生时把信号 connect 一次, 触发时现读该属性。

declare const __JZWidgetObject: any;
declare function init_c_widget(self: any, ...args: any[]): void;
declare function init_c_window(self: any, ...args: any[]): void;
declare function init_c_main_window(self: any, ...args: any[]): void;
declare function init_c_menu_bar(self: any, ...args: any[]): void;
declare function init_c_dialog(self: any, ...args: any[]): void;
declare function init_c_label(self: any, ...args: any[]): void;
declare function init_c_push_button(self: any, ...args: any[]): void;
declare function init_c_line_edit(self: any, ...args: any[]): void;
declare function init_c_checkbox(self: any, ...args: any[]): void;
declare function init_c_radio_button(self: any, ...args: any[]): void;
declare function init_c_slider(self: any, ...args: any[]): void;
declare function init_c_progress_bar(self: any, ...args: any[]): void;
declare function init_c_combo_box(self: any, ...args: any[]): void;
declare function init_c_spin_box(self: any, ...args: any[]): void;
declare function init_c_double_spin_box(self: any, ...args: any[]): void;
declare function init_c_date_edit(self: any, ...args: any[]): void;
declare function init_c_group_box(self: any, ...args: any[]): void;
declare function init_c_switch(self: any, ...args: any[]): void;
declare function init_c_text_edit(self: any, ...args: any[]): void;
declare function init_c_chart(self: any, ...args: any[]): void;
declare function init_c_image_label(self: any, ...args: any[]): void;
declare function init_c_list_widget(self: any, ...args: any[]): void;
declare function init_c_table_widget(self: any, ...args: any[]): void;
declare function init_c_stacked_widget(self: any, ...args: any[]): void;
declare function init_c_tab_widget(self: any, ...args: any[]): void;
declare function init_c_splitter(self: any, ...args: any[]): void;
declare function init_c_scroll_area(self: any, ...args: any[]): void;
declare function init_c_scroll_bar(self: any, ...args: any[]): void;

/** "别造祖先那份底层对象"的哨兵: 派生类构造器里 super(NOCREATE) 用 */
const NOCREATE: unique symbol = Symbol('jz.nocreate');

/** 已经装过的类(幂等用): 见文件末尾的说明 */
const __installed: Record<string, any> | undefined = (globalThis as any).__jzWidgetClasses;

/**
 * 派生类工厂: 定义 "extends parent + super(NOCREATE) + init_c_xxx(this, ...args)" 的类并挂到全局。
 * 只给控件族的派生类用 —— 参数不管顺序、不加工, 原样丢给 C 侧的形态分派。
 */
function defWidgetClass(name: string, parent: any, initName: string): any
{
    if (__installed)
        return __installed[name];   // 幂等: 复用已装好的类(见文件末尾说明)

    class WidgetClass extends parent
    {
        constructor(...args: any[])
        {
            super(NOCREATE);

            // 自己也是被"派生类的 super(NOCREATE)"调用的(super 链会一路传下来): 只建后代的底层对象,
            // 这一份不建 —— 否则 init_c_xxx 会收到 NOCREATE 这个 symbol 当实参, C 侧形态分派直接不匹配。
            if (args.length === 1 && args[0] === NOCREATE)
                return;

            const init = (globalThis as any)[initName];
            if (typeof init !== 'function')
                throw new Error(`${name}: 引擎没有注册 ${initName}(控件类的底层构造函数缺失)`);
            init(this, ...args);
        }
    }

    // 让 constructor.name / 报错信息用 JS 侧类名, 而不是内部的 WidgetClass
    Object.defineProperty(WidgetClass, 'name', { value: name, configurable: true });
    (globalThis as any)[name] = WidgetClass;
    return WidgetClass;
}

/** 基类: 唯一会造"自己那份底层对象"的类(派生类都用 super(NOCREATE) 跳过它) */
const JZWidget: any = __installed ? __installed.JZWidget : (() => {
    class WidgetClass extends (__JZWidgetObject as any)
    {
        constructor(parentOrNoCreate?: any)
        {
            super();
            if (parentOrNoCreate === NOCREATE)
                return;   // 派生类的 super(NOCREATE): 不造这一份

            // ⚠ 无参/显式 null 都要走**零参**重载: `init_c_widget(this, undefined)` 会变成
            //   "1 个 undefined 实参", C 侧形态分派(`null|JZWidget`)匹配不上。
            if (parentOrNoCreate === undefined || parentOrNoCreate === null)
                init_c_widget(this);
            else
                init_c_widget(this, parentOrNoCreate);
        }
    }

    Object.defineProperty(WidgetClass, 'name', { value: 'JZWidget', configurable: true });
    (globalThis as any).JZWidget = WidgetClass;
    return WidgetClass;
})();

const JZWindow      = defWidgetClass('JZWindow',      JZWidget, 'init_c_window');
const JZMainWindow  = defWidgetClass('JZMainWindow',  JZWindow, 'init_c_main_window');
const JZDialog      = defWidgetClass('JZDialog',      JZWindow, 'init_c_dialog');
const JZMenuBar     = defWidgetClass('JZMenuBar',     JZWidget, 'init_c_menu_bar');
const JZLabel       = defWidgetClass('JZLabel',       JZWidget, 'init_c_label');
const JZPushButton  = defWidgetClass('JZPushButton',  JZWidget, 'init_c_push_button');
const JZLineEdit    = defWidgetClass('JZLineEdit',    JZWidget, 'init_c_line_edit');
const JZCheckbox    = defWidgetClass('JZCheckbox',    JZWidget, 'init_c_checkbox');
const JZRadioButton = defWidgetClass('JZRadioButton', JZWidget, 'init_c_radio_button');
const JZSlider      = defWidgetClass('JZSlider',      JZWidget, 'init_c_slider');
const JZProgressBar = defWidgetClass('JZProgressBar', JZWidget, 'init_c_progress_bar');
const JZComboBox    = defWidgetClass('JZComboBox',    JZWidget, 'init_c_combo_box');
const JZSpinBox     = defWidgetClass('JZSpinBox',     JZWidget, 'init_c_spin_box');
// 小数步进框(对齐 Qt QDoubleSpinBox): 与 JZSpinBox 同一形状, 只是值/范围/步长走 double 口径。
// ⚠ JS 侧继承链必须与 C++ 一致(JZDoubleSpinBox : JZSpinBox) —— 同名方法在子类原型上遮蔽父类那份。
const JZDoubleSpinBox = defWidgetClass('JZDoubleSpinBox', JZSpinBox, 'init_c_double_spin_box');
// 日期编辑框(对齐 Qt QDateEdit; C++ 侧实现仍是骨架, 见 JZWidget/JZDateEdit.h):
// C++ 只给 setDateMsecs/dateMsecs 这类"原始口径", Qt 形状的 setDate(Date)/dateTime()
// 由 ./dates.ts 装到本类原型上。
const JZDateEdit    = defWidgetClass('JZDateEdit',    JZLineEdit, 'init_c_date_edit');
const JZGroupBox    = defWidgetClass('JZGroupBox',    JZWidget, 'init_c_group_box');
const JZSwitch      = defWidgetClass('JZSwitch',      JZWidget, 'init_c_switch');
const JZTextEdit    = defWidgetClass('JZTextEdit',    JZWidget, 'init_c_text_edit');
const JZChart       = defWidgetClass('JZChart',       JZWidget, 'init_c_chart');
const JZImageLabel  = defWidgetClass('JZImageLabel',  JZWidget, 'init_c_image_label');
const JZListWidget  = defWidgetClass('JZListWidget',  JZWidget, 'init_c_list_widget');
const JZTableWidget = defWidgetClass('JZTableWidget', JZWidget, 'init_c_table_widget');
const JZStackedWidget = defWidgetClass('JZStackedWidget', JZWidget, 'init_c_stacked_widget');
const JZTabWidget   = defWidgetClass('JZTabWidget',   JZWidget, 'init_c_tab_widget');
const JZSplitter    = defWidgetClass('JZSplitter',    JZWidget, 'init_c_splitter');
const JZScrollArea  = defWidgetClass('JZScrollArea',  JZWidget, 'init_c_scroll_area');
const JZScrollBar   = defWidgetClass('JZScrollBar',   JZWidget, 'init_c_scroll_bar');

// 幂等: 这个模块可能被**求值两次** —— 引擎先按路径加载 classes.js(类层), 之后包入口 index.js
// 再 import './classes'(两条 specifier 不一定命中同一份模块缓存)。第二次必须原样复用:
// 重建类会换掉原型, 而 C++ 挂上去的方法在旧原型上(new 出来的对象就"没有方法"了)。
(globalThis as any).__jzWidgetClasses = __installed || {
    JZWidget, JZWindow, JZMainWindow, JZDialog, JZMenuBar, JZLabel, JZPushButton, JZLineEdit,
    JZDateEdit, JZDoubleSpinBox,
    JZCheckbox, JZRadioButton, JZSlider, JZProgressBar, JZComboBox, JZSpinBox, JZGroupBox,
    JZSwitch, JZTextEdit, JZChart, JZImageLabel, JZListWidget, JZTableWidget, JZStackedWidget,
    JZTabWidget, JZSplitter, JZScrollArea, JZScrollBar,
};

export { JZWidget, JZWindow, JZMainWindow, JZDialog, JZMenuBar, JZLabel, JZPushButton, JZLineEdit,
         JZDateEdit, JZDoubleSpinBox,
         JZCheckbox, JZRadioButton, JZSlider, JZProgressBar, JZComboBox, JZSpinBox, JZGroupBox,
         JZSwitch, JZTextEdit, JZChart, JZImageLabel, JZListWidget, JZTableWidget, JZStackedWidget,
         JZTabWidget, JZSplitter, JZScrollArea, JZScrollBar };
