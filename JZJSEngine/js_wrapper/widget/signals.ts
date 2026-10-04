// @jz/widget —— 把"信号/事件"装成**原型上的属性**(默认 null)。
//
// 为什么需要:
//   C++ 侧 defSignalProperty 只做了一件事 —— 登记"这个类有这个信号", 然后在对象诞生时把信号
//   connect 到一个"每次去 JS 对象上现读同名属性"的 lambda。也就是说:**信号本身不是 JS 属性**
//   (赋值能工作, 因为那就是普通属性; 但 `'onClicked' in btn` 是 false)。
//   而 types/*.d.ts 承诺的是属性(`onClicked: ((...) => void) | null`), 调用方也按属性写
//   (如 bteserver 的 wire(): `if (!(sigProp in w)) 跳过` + `w[sigProp] = cb`) —— 不装属性的话
//   这类"按名字接线"的代码会全部静默跳过。
//
// 做法: 在每个类的原型上装一对 get/set —— 读: 没设过 -> null; 写: 存到实例上的隐藏槽。
// (事件 onPaint/onShow/... 同理: 它们也是"对象上的属性, 派发时现读", C++ 侧同样只登记不装。)

// 本文件只往原型上装属性, 没有需要导出的值(但被 index 的 `export *` 引, 得是个模块)
export {};

declare const JZWidget: any;
declare const JZDialog: any;
declare const JZMenu: any;
declare const JZAction: any;
declare const JZPushButton: any;
declare const JZLineEdit: any;
declare const JZCheckbox: any;
declare const JZRadioButton: any;
declare const JZSlider: any;
declare const JZComboBox: any;
declare const JZSpinBox: any;
declare const JZDoubleSpinBox: any;
declare const JZDateEdit: any;
declare const JZSwitch: any;
declare const JZTextEdit: any;
declare const JZListWidget: any;
declare const JZTableWidget: any;
declare const JZTabWidget: any;
declare const JZSplitter: any;
declare const JZScrollBar: any;

const SLOT = (name: string): string => '__sig_' + name;

function defProp(proto: any, name: string): void
{
    if (!proto || Object.prototype.hasOwnProperty.call(proto, name))
        return;

    Object.defineProperty(proto, name, {
        configurable: true,
        get(this: any) {
            const v = this[SLOT(name)];
            return v === undefined ? null : v;
        },
        set(this: any, v: any) {
            // 写成**实例上的普通数据属性**(不是藏到某个槽里): 派发/读回都走它, 且
            // `Object.prototype.hasOwnProperty.call(w, 'onPaint')` 仍为 true ——
            // 实例属性优先于原型访问器, 这一次赋值之后就与原型那对 get/set 无关了。
            Object.defineProperty(this, name, {
                value: v,
                writable: true,
                enumerable: true,
                configurable: true,
            });
        },
    });
}

function defProps(cls: any, names: string[]): void
{
    if (typeof cls === 'undefined' || !cls || !cls.prototype)
        return;
    for (const n of names)
        defProp(cls.prototype, n);
}

// ---- 事件(JZWidget 上, 所有控件继承) ----
defProps(typeof JZWidget !== 'undefined' ? JZWidget : undefined, [
    'onPaint', 'onMousePress', 'onMouseRelease', 'onMouseMove', 'onWheel',
    'onEnter', 'onLeave', 'onKeyPress', 'onKeyRelease',
    'onFocusIn', 'onFocusOut', 'onShow', 'onHide', 'onResize',
]);

// ---- 信号(与 C++ 侧 defSignalProperty 一一对应) ----
defProps(typeof JZWidget !== 'undefined' ? JZWidget : undefined, ['onCustomContextMenuRequested']);
defProps(typeof JZDialog !== 'undefined' ? JZDialog : undefined, ['onAccepted', 'onRejected', 'onFinished']);
defProps(typeof JZMenu !== 'undefined' ? JZMenu : undefined, ['onActionTriggered']);
defProps(typeof JZAction !== 'undefined' ? JZAction : undefined, ['onTriggered']);
defProps(typeof JZPushButton !== 'undefined' ? JZPushButton : undefined, ['onClicked']);
defProps(typeof JZLineEdit !== 'undefined' ? JZLineEdit : undefined, ['onTextChanged', 'onReturnPressed', 'onEditingFinished']);
defProps(typeof JZCheckbox !== 'undefined' ? JZCheckbox : undefined, ['onToggled']);
defProps(typeof JZRadioButton !== 'undefined' ? JZRadioButton : undefined, ['onToggled']);
defProps(typeof JZSlider !== 'undefined' ? JZSlider : undefined, ['onValueChanged']);
defProps(typeof JZComboBox !== 'undefined' ? JZComboBox : undefined, ['onCurrentIndexChanged', 'onCurrentTextChanged']);
defProps(typeof JZSpinBox !== 'undefined' ? JZSpinBox : undefined, ['onValueChanged']);
// 小数步进框: onValueChanged 与 JZSpinBox 同名(原型链上本来也有), 这里显式装一遍保持"每类都声明"的惯例
defProps(typeof JZDoubleSpinBox !== 'undefined' ? JZDoubleSpinBox : undefined, ['onValueChanged']);
// 日期框: 日期变化回调带一个 Date(见 C++ 侧 ensureConnected(JZSigDateTime)); 文本类信号继承 JZLineEdit
defProps(typeof JZDateEdit !== 'undefined' ? JZDateEdit : undefined, ['onDateChanged']);
defProps(typeof JZSwitch !== 'undefined' ? JZSwitch : undefined, ['onToggled']);
defProps(typeof JZTextEdit !== 'undefined' ? JZTextEdit : undefined, ['onTextChanged']);
defProps(typeof JZListWidget !== 'undefined' ? JZListWidget : undefined, ['onCurrentIndexChanged', 'onActivated', 'onDataChanged']);
defProps(typeof JZTableWidget !== 'undefined' ? JZTableWidget : undefined, ['onCurrentRowChanged', 'onCurrentColumnChanged', 'onDataChanged']);
defProps(typeof JZTabWidget !== 'undefined' ? JZTabWidget : undefined, ['onCurrentChanged']);
defProps(typeof JZSplitter !== 'undefined' ? JZSplitter : undefined, ['onSplitterMoved']);
defProps(typeof JZScrollBar !== 'undefined' ? JZScrollBar : undefined, ['onValueChanged']);
