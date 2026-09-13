// test_jswidget —— test/widget/widget(C++ 控件演示 test_widget) 的 **JS 版**。
//
// 对照关系(左侧列表的 20 项与 C++ 版一一对应):
//   Label / PushButton / Slider / ProgressBar / Switch / Checkbox / RadioButton / ComboBox /
//   LineEdit / TextEdit / Table / ImageLabel / Dialog / TabWidget / Splitter / SpinBox /
//   ScrollBar / GroupBox / ScrollArea / Animation
//
// 运行: code_runtime --root <本目录>      (存在可见顶层窗口, 关窗才退出)
//
// JS 侧与 C++ 侧的差异(受当前绑定能力限制, 详见各页注释):
//   * Dialog: 不能开**模态**对话框(exec 会重入事件循环, JS 栈还压在下面), 改成再开一个
//     独立小窗口当对话框; 原生文件对话框同样暂不可用。
//   * Animation: 没有 JZPropertyAnimation 绑定, 用 setInterval 逐帧 setGeometry 演示同样的效果。
//   * ImageLabel: 图片用 JZImage.create(w,h) 新建 + JZPainter 离屏画上去(替代加载文件)。
//
// ⚠ 所有权与保活(现行规则):
//   * **挂进对象树 = 所有权转移**: addWidget / insertWidget / addTab / setWidget / setParentWidget
//     都会把包装翻成"C++ 拥有"(/Transfer/, cppOwner=true), 之后由父对象树(顶层就是窗口)负责 delete;
//     同时按控件登记到 JZJSWidgetModule —— **父节点持一份"子控件 JS 包装"的引用防 GC**。
//     所以: 挂进树的控件/布局(setLayout 也一样)**不需要**自己 keep, 放手也不会被回收。
//   * **顶层对象归 JS 所有**: JS 放手 -> 包装析构 -> 窗口 delete -> 没窗口了 runtime 退出。
//     故顶层窗口必须留一份引用(下面 win / 对话框)。
//   * 反过来 setParentWidget(null) 会把控件交回 JS 拥有(脱离树)。

const keep = [];   // 只留给"JS 拥有且必须活着"的对象(顶层窗口); 挂进树的控件不需要
function K(obj) { keep.push(obj); return obj; }
// (下面各页里还留着的 K(...) 是旧写法: 那些控件随后都 addWidget 挂进了树, 其实已经不必 keep —— 留着无害)

// ---------------------------------------------------------------------------
// 日志(底部 JZTextEdit, 只读)
// ---------------------------------------------------------------------------

let logEdit = null;
let logLines = [];

function log(line)
{
    logLines.push(line);
    if (logLines.length > 300)
        logLines = logLines.slice(logLines.length - 300);
    if (!logEdit)
        return;
    const text = logLines.join("\n");
    logEdit.setText(text);
    logEdit.setCursorPosition(text.length);   // 滚到末尾
}

// ---------------------------------------------------------------------------
// 换肤: 全局样式表整表替换(变量只有 :root 一处, 组件规则零改动)
// ---------------------------------------------------------------------------

const THEME_ROOT_LIGHT =
    ":root {\n" +
    "    --bg: #ffffff;\n" +
    "    --panel: #f5f7fa;\n" +
    "    --text: #202020;\n" +
    "    --btn: #e0e0e0;\n" +
    "    --btn-border: #808080;\n" +
    "    --accent: #3498db;\n" +
    "}\n";

const THEME_ROOT_DARK =
    ":root {\n" +
    "    --bg: #161a21;\n" +
    "    --panel: #10141a;\n" +
    "    --text: #e6e9ef;\n" +
    "    --btn: #2a3140;\n" +
    "    --btn-border: #4a5364;\n" +
    "    --accent: #4f8cff;\n" +
    "}\n";

// 组件规则: 两个主题完全一致, 只吃令牌
const THEME_RULES =
    "#main { background-color: var(--bg); }\n" +
    "JZLabel { color: var(--text); }\n" +
    "JZPushButton {\n" +
    "    background-color: var(--btn);\n" +
    "    border: 1px solid var(--btn-border);\n" +
    "    border-radius: 4px;\n" +
    "}\n" +
    ".title { color: var(--accent); }\n" +
    ".panel { background-color: var(--panel); border-radius: 6px; }\n";

function applyTheme(dark)
{
    const css = (dark ? THEME_ROOT_DARK : THEME_ROOT_LIGHT) + THEME_RULES;
    try
    {
        jzWidget.setStyleSheet(css);
        return true;
    }
    catch (e)
    {
        log("theme error: " + e.message);
        return false;
    }
}

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

// 新建一页(普通容器 + VBoxLayout), 页面本身由 JZStackedWidget/JZTabWidget 接管显示
// 构造器直接吃实参: JZVBoxLayout(pad, spacing) / JZLabel(text) / JZWindow(w, h, title) ...
function makePage()
{
    const p = new JZWidget();
    const v = new JZVBoxLayout(12, 10);          // 边距 12, 间距 10
    p.setLayout(v);                               // setLayout 后布局归控件所有
    return { page: p, lay: v };
}

// 一行控件(水平布局的容器)
function makeRow()
{
    const r = new JZWidget();
    const h = new JZHBoxLayout(0, 6);             // 无边距, 间距 6
    r.setLayout(h);
    return { row: r, lay: h };
}

function label(text)
{
    return new JZLabel(text);
}

function button(text, onClick, w, h)
{
    const b = (w !== undefined && h !== undefined) ? new JZPushButton(null, w, h, text)
                                                   : new JZPushButton();
    if (w === undefined || h === undefined) b.setText(text);
    if (onClick) b.onClicked(onClick);
    return b;
}

// ---------------------------------------------------------------------------
// 各页构建
// ---------------------------------------------------------------------------

const NAMES = [
    "Label", "PushButton", "Slider", "ProgressBar", "Switch", "Checkbox",
    "RadioButton", "ComboBox", "LineEdit", "TextEdit", "Table", "ImageLabel",
    "Dialog", "TabWidget", "Splitter", "SpinBox", "ScrollBar", "GroupBox",
    "ScrollArea", "Animation"
];

const builders = {};

// ---- Label ----
builders.Label = function () {
    const { page, lay } = makePage();
    const l = label("Hello");
    l.setObjectName("demoLabel");        // viewId: MCP 的 ui_invoke 可用 '#demoLabel' 精确定位
    l.setAlign(1);
    lay.addWidget(l, 0);
    let n = 0;
    const upd = button("Update", () => {
        n++;
        l.setText("updated " + n);
        log("label updated: " + n);
    }, 100, 32);
    upd.setObjectName("demoUpdate");
    lay.addWidget(upd, 0);
    return page;
};

// ---- PushButton ----
builders.PushButton = function () {
    const { page, lay } = makePage();
    lay.addWidget(button("Click Me", () => log("event: clicked"), 120, 44), 0);
    return page;
};

// ---- Slider ----
builders.Slider = function () {
    const { page, lay } = makePage();
    const s = K(new JZSlider());
    s.setObjectName("demoSlider");
    s.setRange(0, 100);
    s.setValue(50);
    s.onValueChanged((v) => log("value: " + v));
    lay.addWidget(s, 0);
    return page;
};

// ---- ProgressBar ----
builders.ProgressBar = function () {
    const { page, lay } = makePage();
    const p = K(new JZProgressBar());
    let val = 60;
    p.setRange(0, 100);
    p.setValue(val);
    lay.addWidget(p, 0);
    lay.addWidget(button("+10", () => {
        val = (val + 10) % 110;
        p.setValue(val);
        log("progress: " + val);
    }, 90, 28), 0);
    return page;
};

// ---- Switch ----
builders.Switch = function () {
    const { page, lay } = makePage();
    const sw = K(new JZSwitch());
    sw.onToggled((on) => log("state: " + (on ? "on" : "off")));
    lay.addWidget(sw, 0);
    return page;
};

// ---- Checkbox ----
builders.Checkbox = function () {
    const { page, lay } = makePage();
    const c = K(new JZCheckbox());
    c.setText("Enable");
    c.onToggled((on) => log("state: " + (on ? "on" : "off")));
    lay.addWidget(c, 0);
    return page;
};

// ---- RadioButton ----
builders.RadioButton = function () {
    const { page, lay } = makePage();
    const r1 = K(new JZRadioButton());
    r1.setText("Option A");
    const r2 = K(new JZRadioButton());
    r2.setText("Option B");
    r1.onToggled(() => log("event: clicked (A)"));
    r2.onToggled(() => log("event: clicked (B)"));
    lay.addWidget(r1, 0);
    lay.addWidget(r2, 0);
    return page;
};

// ---- ComboBox ----
builders.ComboBox = function () {
    const { page, lay } = makePage();
    const cb = K(new JZComboBox());
    cb.addItems(["Red", "Green", "Blue"]);       // 数组形参: 绑定层用 JSValue 收, 见 addItems
    cb.onCurrentIndexChanged((i) => log("value: " + i));
    cb.onCurrentTextChanged((s) => log("text: " + s));
    lay.addWidget(cb, 0);
    return page;
};

// ---- LineEdit ----
builders.LineEdit = function () {
    const { page, lay } = makePage();
    const le = K(new JZLineEdit());
    le.setText("edit me");
    le.setPlaceholderText("type here...");
    le.resize(200, 70);
    le.onTextChanged((s) => log("text: " + s));
    le.onReturnPressed(() => log("event: return pressed"));
    lay.addWidget(le, 0);
    return page;
};

// ---- TextEdit ----
builders.TextEdit = function () {
    const { page, lay } = makePage();
    const te = K(new JZTextEdit());
    te.setText("edit me");
    te.resize(200, 90);
    lay.addWidget(te, 1);
    lay.addWidget(button("Log", () => log("edit: " + te.text()), 90, 28), 0);
    return page;
};

// ---- Table ----
builders.Table = function () {
    const { page, lay } = makePage();
    const tbl = K(new JZTableWidget());
    tbl.setColumnCount(3);
    tbl.setHorizontalHeaderLabels(["Id", "Name", "Val"]);
    lay.addWidget(tbl, 1);

    let seq = 0;
    function appendRows(n)
    {
        const r0 = tbl.rowCount();
        tbl.setRowCount(r0 + n);
        for (let i = 0; i < n; i++)
        {
            seq++;
            tbl.setItemText(r0 + i, 0, "" + seq);
            tbl.setItemText(r0 + i, 1, "item " + seq);
            tbl.setItemText(r0 + i, 2, "" + (seq * 37 % 1000));
        }
        log("table rows: " + tbl.rowCount());
    }
    appendRows(3);

    const { row, lay: hlay } = makeRow();
    hlay.addWidget(button("Add Row", () => appendRows(1), 90, 28), 0);
    hlay.addWidget(button("Add 10 Rows", () => appendRows(10), 110, 28), 0);
    hlay.addWidget(button("Clear", () => {
        tbl.setRowCount(0);
        log("table cleared, rows: 0");
    }, 90, 28), 0);
    hlay.addStretch(1);
    lay.addWidget(row, 0);
    return page;
};

// ---- ImageLabel ----
// JS 侧造图: JZImage.create(w,h)(静态工厂) + 离屏 JZPainter 画内容。
builders.ImageLabel = function () {
    const { page, lay } = makePage();
    const img = JZImage.create(96, 96);
    const p = new JZPainter(img);
    p.fillRect(0, 0, 96, 96, "#2b3a55");
    p.setPen("#ffffff", 2);
    p.drawRect(4, 4, 88, 88);
    p.fillEllipse(24, 24, 48, 48, "#e2c044");
    p.drawText(8, 78, "JS");
    p.end();

    const il = K(new JZImageLabel());
    il.setImage(img);
    il.setScale(1);
    lay.addWidget(label("(JZImage.create + 离屏 JZPainter)"), 0);
    lay.addWidget(il, 0);
    return page;
};

// ---- Dialog ----
// 模态 exec() 会重入事件循环(JS 栈还压在 C++ 调用下面), 故用"独立小窗口"当对话框。
builders.Dialog = function () {
    const { page, lay } = makePage();
    lay.addWidget(label("JS 侧不用模态 exec: 点按钮开一个独立小窗口当对话框"), 0);

    // 顶层窗口(对话框)归 JS 所有: 留一份引用即可 —— 这里只留最近一个, 上一个随 GC 销毁(演示别攒窗口)
    let lastDialog = null;
    function openDialog(text)
    {
        const dlg = new JZWindow(320, 140, "Dialog");
        const v = new JZVBoxLayout(12, 8);          // 边距 12, 间距 8
        dlg.setLayout(v);
        v.addWidget(label(text), 0);
        v.addWidget(button("OK", () => {
            dlg.close();
            log("dialog accepted: " + text);
        }, 80, 26), 0);
        dlg.show();
        lastDialog = dlg;
    }

    const { row, lay: hlay } = makeRow();
    const infoBtn = button("Information", () => openDialog("This is an informational message."), 110, 30);
    infoBtn.setObjectName("dlgInfo");      // viewId: MCP 可用 '#dlgInfo' 开一个独立窗口(测多窗口切换)
    hlay.addWidget(infoBtn, 0);
    hlay.addWidget(button("Warning", () => openDialog("Something might be wrong here."), 100, 30), 0);
    hlay.addWidget(button("Question", () => openDialog("Do you want to continue?"), 100, 30), 0);
    hlay.addStretch(1);
    lay.addWidget(row, 0);

    lay.addWidget(label("原生文件对话框暂未绑定(需要模态循环), 用 fs 的 API 代替"), 0);
    return page;
};

// ---- TabWidget ----
builders.TabWidget = function () {
    const { page, lay } = makePage();
    const tabs = K(new JZTabWidget());
    tabs.onCurrentChanged((i) => log("tab: " + i));
    lay.addWidget(tabs, 1);

    const a = makePage();
    a.lay.addWidget(label("Tab A content"), 0);
    tabs.addTab(a.page, "A");

    const b = makePage();
    b.lay.addWidget(button("Btn in Tab B", () => log("event: clicked"), 120, 32), 0);
    tabs.addTab(b.page, "B");

    const c = makePage();
    const cb = K(new JZCheckbox());
    cb.setText("Checkbox in Tab C");
    cb.onToggled((on) => log("state: " + (on ? "on" : "off")));
    c.lay.addWidget(cb, 0);
    tabs.addTab(c.page, "C");
    return page;
};

// ---- Splitter ----
// 分栏里的"色块面板"= 普通 JZWidget + onPaint 填色(对应 C++ 的 ColorPanel)。
function colorPanel(color)
{
    const w = K(new JZWidget());
    w.onPaint = (e) => {
        const p = new JZPainter(w);
        p.fillRect(0, 0, w.width(), w.height(), color);
    };
    return w;
}

builders.Splitter = function () {
    const { page, lay } = makePage();
    const hsp = K(new JZSplitter());
    hsp.setHandleWidth(6);
    hsp.setStretchFactor(0, 1);
    hsp.setStretchFactor(1, 2);
    hsp.onSplitterMoved((pos, handle) => log("splitter moved: handle=" + handle + " pos=" + pos));

    const left = colorPanel("#add8e6");

    const right = K(new JZSplitter());
    right.setOrientation(1);        // 1 = Vertical(构造实参不生效, 必须显式 set)
    right.setHandleWidth(6);
    right.setStretchFactor(0, 2);
    right.setStretchFactor(1, 1);
    right.onSplitterMoved((pos, handle) => log("splitter moved: handle=" + handle + " pos=" + pos));
    right.addWidget(colorPanel("#90ee90"));
    right.addWidget(colorPanel("#fffacd"));

    hsp.addWidget(left);
    hsp.addWidget(right);

    lay.addWidget(hsp, 1);
    return page;
};

// ---- SpinBox ----
builders.SpinBox = function () {
    const { page, lay } = makePage();
    const sb = K(new JZSpinBox());
    sb.setRange(0, 100);
    sb.setValue(42);
    sb.setSuffix(" %");
    sb.onValueChanged((v) => log("spin value: " + v));
    lay.addWidget(sb, 0);
    return page;
};

// ---- ScrollBar ----
// 4000×4000 虚拟世界 + 两条独立 JZScrollBar 驱动平移, 内容由 JS 在 onPaint 里画
// (对应 C++ 的 VirtualCanvas; 小地图只画不拖, 拖拽逻辑见 C++ 版)。
builders.ScrollBar = function () {
    const { page, lay } = makePage();

    const WORLD = 4000;
    const BAR = 16;
    const canvas = K(new JZWidget());
    canvas.setObjectName("virtualCanvas");
    const hbar = K(new JZScrollBar());
    const vbar = K(new JZScrollBar());
    vbar.setOrientation(1);
    hbar.setRange(0, WORLD);
    vbar.setRange(0, WORLD);
    hbar.onValueChanged((v) => canvas.update());
    vbar.onValueChanged((v) => canvas.update());

    let vx = 0, vy = 0;

    function syncBars()
    {
        const vw = Math.max(0, canvas.width() - BAR);
        const vh = Math.max(0, canvas.height() - BAR);
        hbar.setGeometry(0, canvas.height() - BAR, vw, BAR);
        vbar.setGeometry(canvas.width() - BAR, 0, BAR, vh);
        hbar.setRange(0, Math.max(0, WORLD - vw));
        vbar.setRange(0, Math.max(0, WORLD - vh));
        hbar.setSingleStep(50);
        vbar.setSingleStep(50);
        hbar.setPageStep(vw > 0 ? vw : 1);
        vbar.setPageStep(vh > 0 ? vh : 1);
        if (vx > hbar.maximum()) { vx = hbar.maximum(); hbar.setValue(vx); }
        if (vy > vbar.maximum()) { vy = vbar.maximum(); vbar.setValue(vy); }
    }

    canvas.onResize = () => { syncBars(); };

    canvas.onWheel = (e) => {
        const dir = (e.wheelDelta() > 0) ? -1 : 1;
        vbar.setValue(vbar.value() + dir * vbar.singleStep());
        e.accept();
    };

    // 按住左键拖动 = 平移世界。用"按下/松开"自己记拖动状态: move 事件不携带"哪个键按住"
    // (JZMouseEvent.button() 是"本事件对应的键", 移动事件为 NoButton), 不能靠它判断。
    let dragging = false;
    let lastX = 0, lastY = 0;
    canvas.onMousePress = (e) => {
        if (e.button() !== 1) return;
        dragging = true;
        lastX = e.x();
        lastY = e.y();
    };
    canvas.onMouseRelease = (e) => { dragging = false; };
    canvas.onMouseMove = (e) => {
        if (!dragging)
            return;
        vx = hbar.value() - (e.x() - lastX);
        vy = vbar.value() - (e.y() - lastY);
        if (vx < 0) vx = 0; else if (vx > hbar.maximum()) vx = hbar.maximum();
        if (vy < 0) vy = 0; else if (vy > vbar.maximum()) vy = vbar.maximum();
        lastX = e.x();
        lastY = e.y();
        hbar.setValue(vx);
        vbar.setValue(vy);
    };

    canvas.onPaint = (e) => {
        syncBars();
        vx = hbar.value();
        vy = vbar.value();

        const vw = Math.max(0, canvas.width() - BAR);
        const vh = Math.max(0, canvas.height() - BAR);
        const p = new JZPainter(canvas);
        p.fillRect(0, 0, canvas.width(), canvas.height(), "#e4e8ee");
        p.fillRect(0, 0, vw, vh, "#f6f8fa");

        // 网格(世界坐标 50 一条, 500 一条主线)
        const step = 50, major = 500;
        p.setPen("#e4e8ee", 1);
        for (let wx = vx - (vx % step); wx <= vx + vw; wx += step)
        {
            if (wx % major === 0) continue;
            const x = wx - vx;
            if (x >= 0) p.drawLine(x, 0, x, vh - 1);
        }
        for (let wy = vy - (vy % step); wy <= vy + vh; wy += step)
        {
            if (wy % major === 0) continue;
            const y = wy - vy;
            if (y >= 0) p.drawLine(0, y, vw - 1, y);
        }
        p.setPen("#b0b8c4", 1);
        for (let wx = vx - (vx % major); wx <= vx + vw; wx += major)
        {
            const x = wx - vx;
            if (x < 0) continue;
            p.drawLine(x, 0, x, vh - 1);
            p.drawText(x + 3, 3, "" + wx);
        }
        for (let wy = vy - (vy % major); wy <= vy + vh; wy += major)
        {
            const y = wy - vy;
            if (y < 0) continue;
            p.drawLine(0, y, vw - 1, y);
            p.drawText(3, y + 2, "" + wy);
        }

        // 世界里的固定物件
        const feats = [
            [400, 300, 320, 200, "#81bef0"],
            [2200, 1200, 420, 260, "#94dc9c"],
            [1100, 2600, 360, 260, "#face78"],
        ];
        for (let i = 0; i < feats.length; i++)
        {
            const f = feats[i];
            const x = f[0] - vx, y = f[1] - vy;
            if (x + f[2] <= 0 || y + f[3] <= 0 || x >= vw || y >= vh)
                continue;
            p.fillRect(x, y, f[2], f[3], f[4]);
            p.setPen("#5a6470", 1);
            p.drawRect(x, y, f[2], f[3]);
            p.drawText(x + 10, y + 10, "Region (" + f[0] + ", " + f[1] + ")");
        }

        // 中心指示: 十字 + 对应的世界坐标
        p.setPen("#46505f", 1);
        p.drawLine(vw / 2 - 9, vh / 2, vw / 2 + 9, vh / 2);
        p.drawLine(vw / 2, vh / 2 - 9, vw / 2, vh / 2 + 9);
        p.setPen("#3c3c46", 1);
        p.drawText(vw / 2 + 8, vh / 2 + 6, "center (" + (vx + vw / 2 | 0) + ", " + (vy + vh / 2 | 0) + ")");

        // 右上角小地图(只画)
        const mw = 130, mh = 110;
        const ox = vw - mw - 12, oy = 12;
        if (ox > 40)
        {
            p.fillRect(ox - 4, oy - 4, mw + 8, mh + 8, "#d6dce4");
            p.fillRect(ox, oy, mw, mh, "#fafbfc");
            p.setPen("#969ea8", 1);
            p.drawRect(ox, oy, mw, mh);
            const fx = ox + (vx * mw / WORLD | 0);
            const fy = oy + (vy * mh / WORLD | 0);
            const fw = Math.max(2, vw * mw / WORLD | 0);
            const fh = Math.max(2, vh * mh / WORLD | 0);
            p.fillRect(fx, fy, fw, fh, "#ffd6d6");
            p.setPen("#d03c3c", 1);
            p.drawRect(fx, fy, fw, fh);
            p.setPen("#505864", 1);
            p.drawText(ox, oy + mh + 12, "view " + vx + ", " + vy);
        }
    };

    lay.addWidget(label("拖滚动条 / 滚轮 / 在画面上按住拖动(JS 侧 onPaint 自绘 + onWheel)"), 0);
    lay.addWidget(canvas, 1);
    return page;
};

// ---- GroupBox ----
builders.GroupBox = function () {
    const { page, lay } = makePage();
    const gb = K(new JZGroupBox());
    gb.setTitle("Settings");
    const content = gb.contentWidget();      // 装载子控件的内容区
    const glay = K(new JZVBoxLayout());
    glay.setSpacing(8);
    content.setLayout(glay);

    const cb = K(new JZCheckbox());
    cb.setText("Enable feature");
    glay.addWidget(cb, 0);

    const sb = K(new JZSpinBox());
    sb.setRange(0, 100);
    sb.setValue(10);
    glay.addWidget(sb, 0);

    glay.addWidget(button("Apply", () => log("event: clicked (Apply)"), 100, 28), 0);

    lay.addWidget(gb, 1);
    return page;
};

// ---- ScrollArea ----
builders.ScrollArea = function () {
    const { page, lay } = makePage();

    const area = K(new JZScrollArea());
    lay.addWidget(area, 1);

    // 内容"世界": 固定超大尺寸, 滚动区据它算范围(内容控件由滚动区接管生命周期,
    // 但 JS 这份包装仍要保活 —— 否则 GC 会与滚动区抢着 delete)
    const CELL = 80;
    const COUNT = 12;                 // 12×12 = 144 个按钮(等效 C++ 版的 50×50 缩减版)
    const world = K(new JZWidget());
    world.setFixedSize(CELL * COUNT, CELL * COUNT);
    let id = 0;
    for (let r = 0; r < COUNT; r++)
    {
        for (let c = 0; c < COUNT; c++)
        {
            const num = "" + id;
            const b = button(num, () => log("scroll btn #" + num + " (row=" + r + ", col=" + c + ") clicked"), CELL, CELL);
            b.setObjectName(num);
            b.move(c * CELL, r * CELL);
            b.setParentWidget(world);
            id++;
        }
    }
    area.setWidget(world);

    const vbar = area.verticalScrollBar();
    const hbar = area.horizontalScrollBar();
    vbar.onValueChanged((v) => log("scroll y: " + v));
    hbar.onValueChanged((v) => log("scroll x: " + v));

    lay.addWidget(button("scrollTo(800, 1200)", () => area.scrollTo(800, 1200), 160, 28), 0);
    return page;
};

// ---- Animation ----
// C++ 版用 JZAnimationGroup/JZPropertyAnimation(未绑定); JS 侧用 setInterval 逐帧写 x/y/w/h。
builders.Animation = function () {
    const { page, lay } = makePage();
    lay.addWidget(label("动画方块: JS setInterval 逐帧 setGeometry (x/y/w/h, A<->B)"), 0);

    const stage = K(new JZWidget());
    stage.onPaint = (e) => {
        const p = new JZPainter(stage);
        p.fillRect(0, 0, stage.width(), stage.height(), "#fafbfd");
        p.setPen("#e7ebf1", 1);
        for (let x = 50; x < stage.width(); x += 50)
            p.drawLine(x, 0, x, stage.height());
        for (let y = 50; y < stage.height(); y += 50)
            p.drawLine(0, y, stage.width(), y);
        p.end();
    };

    const box = button("box", () => log("anim box clicked at x=" + box.x() + " y=" + box.y()), 96, 96);
    box.move(24, 24);
    box.setParentWidget(stage);

    let timer = null;
    function stopAnim() { if (timer !== null) { clearInterval(timer); timer = null; } }

    lay.addWidget(button("动画: 移动+缩放", () => {
        stopAnim();
        const pad = 24, wa = 96, ha = 96, wb = 176, hb = 176;
        const xa = pad, ya = pad;
        let xb = stage.width() - wb - pad; if (xb < pad) xb = pad;
        let yb = stage.height() - hb - pad; if (yb < pad) yb = pad;

        const tol = 6;
        const atA = Math.abs(box.x() - xa) <= tol && Math.abs(box.y() - ya) <= tol
                 && Math.abs(box.width() - wa) <= tol && Math.abs(box.height() - ha) <= tol;

        const fx = box.x(), fy = box.y(), fw = box.width(), fh = box.height();
        const tx = atA ? xb : xa, ty = atA ? yb : ya;
        const tw = atA ? wb : wa, th = atA ? hb : ha;

        const t0 = Date.now();
        const DUR = 1500;
        log("anim start: " + (atA ? "A->B" : "B->A")
            + " x " + fx + "->" + tx + ", y " + fy + "->" + ty
            + ", w " + fw + "->" + tw + ", h " + fh + "->" + th);
        timer = setInterval(() => {
            let k = (Date.now() - t0) / DUR;
            if (k >= 1) { k = 1; stopAnim(); }
            const e2 = k * k * (3 - 2 * k);      // 缓入缓出
            box.setGeometry(fx + (tx - fx) * e2 | 0, fy + (ty - fy) * e2 | 0,
                            fw + (tw - fw) * e2 | 0, fh + (th - fh) * e2 | 0);
            stage.update();
            if (k >= 1)
                log("anim finished");
        }, 16);
    }, 150, 30), 0);

    lay.addWidget(stage, 1);
    return page;
};

// ---------------------------------------------------------------------------
// 主窗口装配(对照 C++ MainWindow::setupUi)
// ---------------------------------------------------------------------------

// 顶层窗口: 归 JS 所有 —— 放手就销毁(窗口没了 runtime 退出), 必须留一份引用
const win = K(new JZWindow(900, 660, "JZWidget Widgets (JS)"));
win.setObjectName("main");

// 布局与控件: 挂进树之后由 C++ 对象树持有(+ 模块保活其 JS 包装), 所以不必再 keep
const rootLay = new JZVBoxLayout(6, 4);      // 边距 6, 间距 4
win.setLayout(rootLay);

// 顶部工具条: 换肤 + 说明
const bar = new JZWidget();
bar.setStyleClass("panel");
rootLay.addWidget(bar, 0);
const barLay = new JZHBoxLayout(0, 6);
bar.setLayout(barLay);

let dark = false;
const themeBtn = button("主题: 浅色", () => {
    dark = !dark;
    if (applyTheme(dark))
    {
        themeBtn.setText(dark ? "主题: 深色" : "主题: 浅色");
        log("theme -> " + (dark ? "dark" : "light") + " (整表替换)");
    }
}, 120, 30);
themeBtn.setObjectName("themeBtn");      // viewId: MCP 可用 '#themeBtn' 精确点击
barLay.addWidget(themeBtn, 0);

const tip = label("JS 版控件演示: 左侧选页, 右下角是日志");
tip.setStyleClass("title");
barLay.addWidget(tip, 1);

// 中部: 左列表 + 右 StackedWidget
const mid = new JZWidget();
rootLay.addWidget(mid, 1);
const midLay = new JZHBoxLayout(0, 4);
mid.setLayout(midLay);

const list = new JZListWidget();
list.setObjectName("navList");           // viewId: MCP 可用 select 驱动切页
list.setMinimumWidth(160);
list.onCurrentIndexChanged((i) => {
    stack.setCurrentIndex(i);
    log("select: " + (NAMES[i] === undefined ? "" : NAMES[i]));
});
midLay.addWidget(list, 0);

const stack = new JZStackedWidget();
midLay.addWidget(stack, 1);

// 底部日志
logEdit = new JZTextEdit();
logEdit.setObjectName("log");            // viewId: MCP 可读日志文本核对行为
logEdit.setReadOnly(true);
rootLay.addWidget(logEdit, 0);

// 逐页构建并登记
let built = 0, failed = 0;
for (let i = 0; i < NAMES.length; i++)
{
    const name = NAMES[i];
    const build = builders[name];
    if (!build)
    {
        log("page missing: " + name);
        failed++;
        continue;
    }
    try
    {
        stack.addWidget(build());
        built++;
    }
    catch (err)
    {
        // 某一页失败不影响其它页(绑不上/参数不匹配都会在这里暴露成日志)
        const { page: ep, lay: el } = makePage();
        el.addWidget(label("page '" + name + "' failed: " + err.message), 0);
        stack.addWidget(ep);
        log("page error [" + name + "]: " + err.message);
        failed++;
    }
}

list.appendItems(NAMES);
list.setCurrentIndex(0);
stack.setCurrentIndex(0);

if (!applyTheme(false))
    log("initial theme failed");

win.show();
log("ready. select a widget on the left. pages=" + NAMES.length);
console.log("[test_jswidget] pages built: " + built + ", failed: " + failed);
if (failed > 0)
    console.error("[test_jswidget] 有 " + failed + " 页构建失败, 详见窗口右下角日志");
