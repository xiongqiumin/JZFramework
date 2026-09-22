#include "main_window.h"
#include "JZApplication.h"
#include "JZDialog.h"
#include "JZMessageBox.h"
#include "JZFileDialog.h"
#include "JZDebug.h"
#include "JZLayout.h"
#include "JZLineEdit.h"

// 主窗口尺寸 (原 lv_obj 百分比由固定像素近似)
static constexpr int kW = 800;
static constexpr int kH = 600;

// 文本对齐 (原 LV_TEXT_ALIGN_*)
enum { JZAlignLeft = 0, JZAlignCenter = 1, JZAlignRight = 2 };

// Animation 演示舞台: 浅色底 + 网格, 让被动画方块的移动/缩放轨迹可观察
class DemoStage : public JZWidget
{
public:
    explicit DemoStage(JZWidget* parent = nullptr)
        : JZWidget(parent)
    {}

    void paintEvent(JZPainterEvent* event) override
    {
        (void)event;
        JZPainter p(this);
        p.fillRect(rect(), JZColor(250, 251, 253));
        p.setPen(JZPen(JZColor(231, 235, 241), 1));
        const int step = 50;
        for (int x = step; x < width(); x += step)
            p.drawLine(x, 0, x, height());
        for (int y = step; y < height(); y += step)
            p.drawLine(0, y, width(), y);
    }
};

MainWindow::MainWindow(JZWidget* parent)
    : JZWindow(parent)
{
    m_names = {
        "Label", "PushButton", "Slider", "ProgressBar", "Switch", "Checkbox",
        "RadioButton", "ComboBox", "LineEdit", "TextEdit", "ListWidget", "TableWidget",
        "Chart", "ImageLabel", "Dialog", "TabWidget", "Splitter", "SpinBox", "ScrollBar",
        "GroupBox", "ScrollArea", "Animation"
    };

    setupUi();
}

MainWindow::~MainWindow()
{
}

// ==================== VirtualCanvas: 4000×4000 虚拟画布 + 滚动条 ====================

VirtualCanvas::VirtualCanvas(JZWidget* parent)
    : JZWidget(parent)
{
    // 两条滚动条挂在本控件内: 拖动/点轨道/滚轮 -> 值变化 -> onXScroll 更新偏移
    m_hbar = new JZScrollBar(JZScrollBar::Horizontal, this);
    m_vbar = new JZScrollBar(JZScrollBar::Vertical, this);
    m_hbar->setRange(0, kWorldW);
    m_vbar->setRange(0, kWorldH);
    connect(&m_hbar->sigValueChanged, this, &VirtualCanvas::onHScroll);
    connect(&m_vbar->sigValueChanged, this, &VirtualCanvas::onVScroll);
}

void VirtualCanvas::onHScroll(int v)
{
    if (m_vx == v)
        return;
    m_vx = v;      // 滚动条消息 -> 平移可视窗口
    update();      // 重绘: 世界坐标偏移后内容/中心指示/小地图全部变化
}

void VirtualCanvas::onVScroll(int v)
{
    if (m_vy == v)
        return;
    m_vy = v;
    update();
}

// 可视窗口尺寸变化 (布局/窗口缩放) -> 重新摆放滚动条并同步范围/步长
void VirtualCanvas::resizeEvent(JZResizeEvent* event)
{
    int w = width(), h = height();
    int vw = viewW(), vh = viewH();

    // 横条贴底占右侧宽度, 纵条贴右占下方高度
    if (w >= kBarTh && h >= kBarTh) {
        m_hbar->setGeometry(0, h - kBarTh, vw, kBarTh);
        m_vbar->setGeometry(w - kBarTh, 0, kBarTh, vh);
    }

    // 范围 = 世界总长 - 可视窗口 (拖到哪就到世界哪个位置), 页步=一窗, 单步=一格
    m_hbar->setRange(0, vw > 0 ? kWorldW - vw : 0);
    m_vbar->setRange(0, vh > 0 ? kWorldH - vh : 0);
    m_hbar->setSingleStep(50);
    m_vbar->setSingleStep(50);
    m_hbar->setPageStep(vw > 0 ? vw : 1);
    m_vbar->setPageStep(vh > 0 ? vh : 1);

    // 范围收窄时把越界的偏移收回来
    if (m_vx > m_hbar->maximum()) m_vx = m_hbar->maximum();
    if (m_vy > m_vbar->maximum()) m_vy = m_vbar->maximum();
    if (m_hbar->value() != m_vx) m_hbar->setValue(m_vx);
    if (m_vbar->value() != m_vy) m_vbar->setValue(m_vy);

    update();
    JZWidget::resizeEvent(event);
}

// ---- 小地图几何 (与 paintEvent 完全一致: 右上一块 130×110, 距内容区 12px) ----
bool VirtualCanvas::miniRect(int& ox, int& oy, int& mw, int& mh) const
{
    mw = 130;
    mh = 110;
    oy = 12;
    ox = viewW() - mw - 12;
    return ox > 40;   // 画布太窄不画/不可交互 (与 paintEvent 一致)
}

// 当前可视窗口在小地图内的红框: 世界坐标按同一比例缩到小地图
void VirtualCanvas::miniFrame(const int ox, const int oy, const int mw, const int mh,
                              int& fx, int& fy, int& fw, int& fh) const
{
    const double sx = mw / (double)kWorldW, sy = mh / (double)kWorldH;
    fx = ox + (int)(m_vx * sx);
    fy = oy + (int)(m_vy * sy);
    fw = (int)(viewW() * sx);
    if (fw < 2) fw = 2;
    fh = (int)(viewH() * sy);
    if (fh < 2) fh = 2;
}

bool VirtualCanvas::overMini(const JZPoint& lp) const
{
    int ox, oy, mw, mh;
    return miniRect(ox, oy, mw, mh)
        && lp.x() >= ox && lp.x() <= ox + mw
        && lp.y() >= oy && lp.y() <= oy + mh;
}

// 悬停小地图提示"可拖动"(四向箭头), 离开则取消显式光标 (继承/箭头)
void VirtualCanvas::updateHoverCursor(const JZPoint& lp)
{
    if (overMini(lp))
        setCursor(JZCursor(JZCursorShape::SizeAll));
    else
        unsetCursor();
}

// 写可视原点并同步滚动条。滚动条是值的源头/展示, 三方 (小地图/滚动条/画布)
// 必须始终一致; setValue 同值不发信号、onXScroll 见同值也早退, 不会循环。
void VirtualCanvas::setView(int vx, int vy)
{
    int mxv = kWorldW - viewW();   // 与 resizeEvent 里 setRange 的范围一致
    int myv = kWorldH - viewH();
    if (mxv < 0) mxv = 0;
    if (myv < 0) myv = 0;
    if (vx < 0) vx = 0; else if (vx > mxv) vx = mxv;
    if (vy < 0) vy = 0; else if (vy > myv) vy = myv;

    if (vx == m_vx && vy == m_vy)
        return;
    m_vx = vx;
    m_vy = vy;
    m_hbar->setValue(vx);
    m_vbar->setValue(vy);
    update();
}

// 把小地图上光标位置换算成目标可视原点:
//   dragMode=0 (点空白/连续跟随): 可视窗口中心对准光标下的世界点 -> 点击即"跳到那里";
//   dragMode=1 (按在红框上): 保持按下点到红框的相对偏移 -> 红框 1:1 跟手拖动。
// 指针拖出小地图时夹回边界, 拖动仍有效。
void VirtualCanvas::panTo(const JZPoint& lp)
{
    int ox, oy, mw, mh;
    if (!miniRect(ox, oy, mw, mh))
        return;
    int mx = lp.x() - ox;
    int my = lp.y() - oy;
    if (mx < 0) mx = 0; else if (mx > mw) mx = mw;
    if (my < 0) my = 0; else if (my > mh) my = mh;

    int wx, wy;
    if (m_dragMode == 1) {
        int left = mx - m_grabDx;
        int top  = my - m_grabDy;
        if (left < 0) left = 0; else if (left > mw) left = mw;
        if (top  < 0) top  = 0; else if (top  > mh) top  = mh;
        wx = (int)(left * (double)kWorldW / mw);
        wy = (int)(top  * (double)kWorldH / mh);
    } else {
        wx = (int)(mx * (double)kWorldW / mw) - viewW() / 2;
        wy = (int)(my * (double)kWorldH / mh) - viewH() / 2;
    }
    setView(wx, wy);
}

// 小地图点击/拖动 -> 滚动条跟随 (值的源头是滚动条, 这里反向驱动它)
void VirtualCanvas::mousePressEvent(JZMouseEvent* event)
{
    if (event->button() == JZMouseEvent::LeftButton && overMini(event->localPos())) {
        const JZPoint& lp = event->localPos();
        int ox, oy, mw, mh;
        miniRect(ox, oy, mw, mh);
        int fx, fy, fw, fh;
        miniFrame(ox, oy, mw, mh, fx, fy, fw, fh);
        // 按在红框上 = 拖红框 (保持偏移); 否则 = 跳转 (中心对准) 并连续跟随
        m_dragMode = (lp.x() >= fx && lp.x() <= fx + fw &&
                      lp.y() >= fy && lp.y() <= fy + fh) ? 1 : 0;
        m_grabDx = lp.x() - fx;
        m_grabDy = lp.y() - fy;
        m_dragging = true;
        grabMouse();   // 按住拖出小地图/控件仍持续收到 move/up
        panTo(lp);
        return;
    }
    JZWidget::mousePressEvent(event);
}

void VirtualCanvas::mouseMoveEvent(JZMouseEvent* event)
{
    if (m_dragging) {
        panTo(event->localPos());
        return;
    }
    updateHoverCursor(event->localPos());
}

void VirtualCanvas::mouseReleaseEvent(JZMouseEvent* event)
{
    if (m_dragging && event->button() == JZMouseEvent::LeftButton) {
        m_dragging = false;
        m_dragMode = 0;
        releaseMouse();
        updateHoverCursor(event->localPos());
        return;
    }
    JZWidget::mouseReleaseEvent(event);
}

void VirtualCanvas::wheelEvent(JZMouseEvent* event)
{
    if (event->wheelDelta() == 0) {
        JZWidget::wheelEvent(event);
        return;
    }
    // 滚轮默认纵向 (与 JZScrollArea 一致: 上滚=内容上移)
    const int dir = (event->wheelDelta() > 0) ? -1 : 1;
    m_vbar->setValue(m_vbar->value() + dir * m_vbar->singleStep());
    event->accept();
}

void VirtualCanvas::paintEvent(JZPainterEvent* event)
{
    (void)event;
    JZPainter p(this);
    const int vw = viewW(), vh = viewH();
    if (vw <= 0 || vh <= 0)
        return;

    // 整底铺浅灰 (覆盖右下滚动条凹槽区/交接角), 再在内容区填画布底色
    p.fillRect(0, 0, width(), height(), JZColor(228, 232, 238));
    p.fillRect(0, 0, vw, vh, JZColor(246, 248, 250));

    // ---- 网格: 世界坐标每 50 一条细线 / 每 500 一条主线 + 坐标标号 ----
    // 平移映射: 世界坐标 wx 画在屏幕 x = wx - m_vx (滚动拖动即内容移动)
    const int step = 50, major = 500;
    p.setPen(JZPen(JZColor(228, 232, 238), 1));
    for (int wx = m_vx - m_vx % step; wx <= m_vx + vw; wx += step) {
        if (wx % major == 0) continue;
        int x = wx - m_vx;
        if (x < 0) continue;
        p.drawLine(x, 0, x, vh - 1);
    }
    for (int wy = m_vy - m_vy % step; wy <= m_vy + vh; wy += step) {
        if (wy % major == 0) continue;
        int y = wy - m_vy;
        if (y < 0) continue;
        p.drawLine(0, y, vw - 1, y);
    }
    for (int wx = m_vx - m_vx % major; wx <= m_vx + vw; wx += major) {
        int x = wx - m_vx;
        if (x < 0) continue;
        p.setPen(JZPen(JZColor(176, 184, 196), 1));
        p.drawLine(x, 0, x, vh - 1);
        p.setFont(JZFont(11));
        p.setPen(JZPen(JZColor(132, 140, 152), 1));
        p.drawText(x + 3, 3, std::to_string(wx).c_str());
    }
    for (int wy = m_vy - m_vy % major; wy <= m_vy + vh; wy += major) {
        int y = wy - m_vy;
        if (y < 0) continue;
        p.setPen(JZPen(JZColor(176, 184, 196), 1));
        p.drawLine(0, y, vw - 1, y);
        p.setFont(JZFont(11));
        p.setPen(JZPen(JZColor(132, 140, 152), 1));
        p.drawText(3, y + 2, std::to_string(wy).c_str());
    }

    // ---- 世界中的固定物件 (拖动后明显看到它们随世界平移) ----
    struct Feat { int x, y, w, h; JZColor c; const char* name; };
    static const Feat feats[] = {
        {  400,  300, 320, 200, JZColor(129, 190, 240), "A" },
        { 2200, 1200, 420, 260, JZColor(148, 220, 156), "B" },
        { 1100, 2600, 360, 260, JZColor(250, 206, 120), "C" },
        { 3100,  500, 300, 210, JZColor(238, 160, 156), "D" },
    };
    for (const Feat& f : feats) {
        int x = f.x - m_vx, y = f.y - m_vy;
        if (x + f.w <= 0 || y + f.h <= 0 || x >= vw || y >= vh)
            continue;
        p.fillRect(x, y, f.w, f.h, f.c);
        p.setPen(JZPen(JZColor(90, 100, 112), 1));
        p.drawRect(x, y, f.w, f.h);
        p.setFont(JZFont(15));
        p.setPen(JZPen(JZColor(35, 45, 60), 1));
        std::string cap = std::string("Region ") + f.name +
                          "  (" + std::to_string(f.x) + ", " + std::to_string(f.y) + ")";
        p.drawText(x + 10, y + 10, cap.c_str());
    }

    // 世界正中心基准块 (2000,2000)
    {
        int x = 2000 - m_vx - 50, y = 2000 - m_vy - 50;
        if (x + 100 > 0 && y + 100 > 0 && x < vw && y < vh) {
            p.fillRect(x, y, 100, 100, JZColor(126, 128, 210));
            p.setPen(JZPen(JZColor(80, 82, 150), 1));
            p.drawRect(x, y, 100, 100);
            p.setFont(JZFont(12));
            p.setPen(JZPen(JZColor(250, 250, 255), 1));
            p.drawText(x + 8, y + 42, "world center");
        }
    }

    // ---- 屏幕中央位置指示: 十字对准画面中心, 气泡显示它对应的世界坐标 ----
    // 拖动滚动条时十字下的内容移动, 气泡里的世界坐标不断变化 -> 直观看到"当前位置"
    {
        int cx = vw / 2, cy = vh / 2;
        int wx = m_vx + cx, wy = m_vy + cy;
        p.setPen(JZPen(JZColor(70, 80, 95), 1));
        p.drawLine(cx - 9, cy, cx + 9, cy);
        p.drawLine(cx, cy - 9, cx, cy + 9);

        std::string msg = "center (" + std::to_string(wx) + ", " + std::to_string(wy) + ")";
        int tw = (int)msg.size() * 7 + 16;
        int bx = cx - tw / 2;
        int by = cy + 12;
        if (bx < 2) bx = 2;
        if (bx + tw > vw - 2) bx = vw - 2 - tw;
        if (bx < 2) bx = 2;
        p.fillRect(bx, by, tw, 20, JZColor(255, 255, 255));
        p.setPen(JZPen(JZColor(196, 60, 60), 1));
        p.drawRect(bx, by, tw, 20);
        p.setPen(JZPen(JZColor(60, 60, 70), 1));
        p.setFont(JZFont(12));
        p.drawText(bx + 8, by + 4, msg.c_str());
    }

    // ---- 右上角小地图: 全图 = 整个世界, 红框 = 当前可视窗口的位置 ----
    // 几何/红框换算与鼠标交互共用 miniRect/miniFrame, 保持所见即所得
    {
        int ox, oy, mw, mh;
        if (miniRect(ox, oy, mw, mh)) {
            p.fillRect(ox - 4, oy - 4, mw + 8, mh + 8, JZColor(214, 220, 228));
            p.fillRect(ox, oy, mw, mh, JZColor(250, 251, 252));
            p.setPen(JZPen(JZColor(150, 158, 168), 1));
            p.drawRect(ox, oy, mw, mh);

            int fx, fy, fw, fh;
            miniFrame(ox, oy, mw, mh, fx, fy, fw, fh);
            p.fillRect(fx, fy, fw, fh, JZColor(255, 214, 214));
            p.setPen(JZPen(JZColor(208, 60, 60), 1));
            p.drawRect(fx, fy, fw, fh);

            std::string t = "view " + std::to_string(m_vx) + ", " + std::to_string(m_vy);
            p.setPen(JZPen(JZColor(80, 88, 100), 1));
            p.setFont(JZFont(11));
            p.drawText(ox, oy + mh + 12, t.c_str());
        }
    }
}

void MainWindow::setupUi()
{
    setObjectName("main");   // 样式表用 #main 命中本窗口

    // 顶部工具条: 换肤 (整表替换)
    JZWidget* bar = new JZWidget(this);
    bar->setStyleClass("panel");   // 通用容器也吃样式 (JZWidget 基类画样式盒子)
    JZHBoxLayout* barLayout = new JZHBoxLayout(6, 4);

    // 换肤按钮: 必须 addWidget 进工具条布局 —— 只 new 出来(无父、不在布局里)就不在对象树上,
    // 渲染/命中都看不到它(这正是"最上角切换样式的按钮没了"的原因)。
    m_themeBtn = new JZPushButton("主题: 浅色", nullptr);
    m_themeBtn->setFixedSize(120, 30);
    barLayout->addWidget(m_themeBtn, 0);
    barLayout->addStretch(1);
    bar->setLayout(barLayout);
    connect(&m_themeBtn->sigClicked, this, &MainWindow::onToggleTheme);

    // 中间容器: 左列表 + 右预览
    m_top = new JZWidget(this);

    m_list = new JZListWidget(m_top);
    m_list->appendItems(m_names);
    // 宽度由 sizeHint 决定 (贴合最宽项), 这里只给下限: 内容更宽时自动加宽
    m_list->setMinimumWidth(150);

    m_preview = new JZStackedWidget(m_top);
    m_preview->resize(kW, kH);

    JZHBoxLayout* topLayout = new JZHBoxLayout(4, 4);
    topLayout->addWidget(m_list, 0);    // 固定宽
    topLayout->addWidget(m_preview, 1); // 占剩余宽度
    m_top->setLayout(topLayout);

    // 底部日志
    m_log = new JZTextEdit(this);
    m_log->resize(kW, 120);

    // 主布局: 上(工具条 + 列表/预览) + 下(日志)
    JZVBoxLayout* mainLayout = new JZVBoxLayout(4, 4);
    mainLayout->addWidget(bar, 0);      // 工具条
    mainLayout->addWidget(m_top, 1);    // 占主要空间
    mainLayout->addWidget(m_log, 0);    // 固定高
    setLayout(mainLayout);

    buildPages();
    m_list->setCurrentIndex(0);
    connect(&m_list->currentIndexChanged, this, &MainWindow::onSelectChanged);

    applyTheme(false);   // 初始: 浅色主题
    log("ready. select a widget on the left.");
}

// ==================== 换肤 (整表替换) ====================
//
// 变量仅 :root 作用域, 且是 parse 期文本替换 (无继承 / 无作用域覆盖),
// 所以换主题只能"重设整份样式表"。下面两份 CSS 只有 :root 块不同,
// 组件规则 (kThemeRules) 一字不改 —— 即"仅覆盖令牌, 组件规则零改动"。
static const char* kThemeRootLight =
":root {\n"
"    --bg: #ffffff;\n"
"    --panel: #f5f7fa;\n"
"    --text: #202020;\n"
"    --border: #d4dbe6;\n"
"    --btn: #e0e0e0;\n"
"    --btn-border: #808080;\n"
"    --btn-text: #202020;\n"
"    --accent: #3498db;\n"
"}\n";

static const char* kThemeRootDark =
":root {\n"
"    --bg: #161a21;\n"
"    --panel: #10141a;\n"
"    --text: #e6e9ef;\n"
"    --border: #3a414d;\n"
"    --btn: #2a3140;\n"
"    --btn-border: #4a5364;\n"
"    --btn-text: #e6e9ef;\n"
"    --accent: #4f8cff;\n"
"}\n";

// 组件规则: 两个主题完全一致, 只吃令牌
static const char* kThemeRules =
"#main { background-color: var(--bg); }\n"
"JZLabel { color: var(--text); }\n"
"JZPushButton {\n"
"    background-color: var(--btn);\n"
"    border: 1px solid var(--btn-border);\n"
"    color: var(--btn-text);\n"
"    border-radius: 4px;\n"
"}\n"
".title { color: var(--accent); }\n"
".panel { background-color: var(--panel); border-radius: 6px; }\n";

void MainWindow::applyTheme(bool dark)
{
    m_dark = dark;
    std::string css = std::string(dark ? kThemeRootDark : kThemeRootLight) + kThemeRules;
    if (!jzApp->setStyleSheet(css))
        log("theme error: " + jzApp->styleSheetError());
}

void MainWindow::onToggleTheme()
{
    applyTheme(!m_dark);
    if (m_themeBtn)
        m_themeBtn->setText(m_dark ? "主题: 深色" : "主题: 浅色");
    log(std::string("theme -> ") + (m_dark ? "dark" : "light") + " (整表替换)");
}

JZWidget* MainWindow::makePage()
{
    JZWidget* page = new JZWidget(m_preview);
    page->setLayout(new JZVBoxLayout(10, 10));
    return page;
}

void MainWindow::log(const std::string& line)
{
    m_logText += line + "\n";
    if (m_logText.size() > 800)
        m_logText = m_logText.substr(m_logText.size() - 800);
    m_log->setText(m_logText);
}

void MainWindow::onSelectChanged(int idx)
{
    if (idx < 0 || idx >= (int)m_names.size()) return;
    m_preview->setCurrentIndex(idx);
    log("select: " + m_names[idx]);
}

void MainWindow::onLogVoid()        { log("event: clicked"); }
void MainWindow::onLogInt(int v)    { log("value: " + std::to_string(v)); }
void MainWindow::onLogBool(bool b)  { log("state: " + std::string(b ? "on" : "off")); }
void MainWindow::onLogString(const std::string& s) { log("text: " + s); }

void MainWindow::onPbInc()
{
    m_pbVal = (m_pbVal + 10) % 110;
    if (m_pb) m_pb->setValue(m_pbVal, true);
    log("progress: " + std::to_string(m_pbVal));
}

void MainWindow::onLogTextEdit()
{
    if (m_te)
        log("edit: " + m_te->text());
}

void MainWindow::onLabelUpdate()
{
    static int n = 0;
    n++;
    if (m_labelDemo)
        m_labelDemo->setText("updated " + std::to_string(n));
    log("label updated: " + std::to_string(n));
}

void MainWindow::buildPages()
{
    // ---- Label ----
    {
        JZWidget* page = makePage();
        JZLabel* l = new JZLabel(page);
        l->setAlign(JZAlignCenter);
        l->setText("Hello");
        m_labelDemo = l;
        JZPushButton* b = new JZPushButton("Update");
        connect(&b->sigClicked, this, &MainWindow::onLabelUpdate);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(l);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(b);
        m_preview->addWidget(page);
    }

    // ---- PushButton ----
    {
        JZWidget* page = makePage();
        JZPushButton* b = new JZPushButton("Click Me");
        connect(&b->sigClicked, this, &MainWindow::onLogVoid);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(b);
        m_preview->addWidget(page);
    }

    // ---- Slider ----
    {
        JZWidget* page = makePage();
        JZSlider* s = new JZSlider(page);
        s->setRange(0, 100);
        s->setValue(50);
        s->resize(150, s->height());
        connect(&s->sigValueChanged, this, &MainWindow::onLogInt);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(s);
        m_preview->addWidget(page);
    }

    // ---- ProgressBar ----
    {
        JZWidget* page = makePage();
        JZProgressBar* p = new JZProgressBar(page);
        m_pb = p;
        m_pbVal = 60;
        p->setRange(0, 100);
        p->setValue(m_pbVal, true);
        p->resize(150, p->height());
        JZPushButton* b = new JZPushButton("+10");
        connect(&b->sigClicked, this, &MainWindow::onPbInc);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(p);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(b);
        m_preview->addWidget(page);
    }

    // ---- Switch ----
    {
        JZWidget* page = makePage();
        JZSwitch* sw = new JZSwitch(page);
        connect(&sw->sigToggled, this, &MainWindow::onLogBool);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(sw);
        m_preview->addWidget(page);
    }

    // ---- Checkbox ----
    {
        JZWidget* page = makePage();
        JZCheckbox* c = new JZCheckbox(page);
        c->setText("Enable");
        connect(&c->sigToggled, this, &MainWindow::onLogBool);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(c);
        m_preview->addWidget(page);
    }

    // ---- RadioButton ----
    {
        JZWidget* page = makePage();
        JZRadioButton* r1 = new JZRadioButton("Option A", page);
        JZRadioButton* r2 = new JZRadioButton("Option B", page);
        connect(&r1->sigToggled, this, &MainWindow::onLogVoid);
        connect(&r2->sigToggled, this, &MainWindow::onLogVoid);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(r1);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(r2);
        m_preview->addWidget(page);
    }

    // ---- ComboBox ----
    {
        JZWidget* page = makePage();
        JZComboBox* cb = new JZComboBox(page);
        cb->addItems({ "Red", "Green", "Blue" });
        connect(&cb->currentIndexChanged, this, &MainWindow::onLogInt);
        connect(&cb->currentTextChanged, this, &MainWindow::onLogString);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(cb);
        m_preview->addWidget(page);
    }
    
    // ---- LineEdit ----
    {
        JZWidget* page = makePage();
        JZLineEdit* line_edit = new JZLineEdit(page);
        line_edit->setText("edit me");
        line_edit->resize(160, 70);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(line_edit);
        m_preview->addWidget(page);
    }
    // ---- TextEdit ----
    {
        JZWidget* page = makePage();
        JZTextEdit* te = new JZTextEdit(page);
        m_te = te;
        te->setText("edit me");
        te->resize(160, 70);
        JZPushButton* b = new JZPushButton("Log");
        connect(&b->sigClicked, this, &MainWindow::onLogTextEdit);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(te);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(b);
        m_preview->addWidget(page);
    }

    // ---- ListWidget ----
    {
        JZWidget* page = makePage();
        JZVBoxLayout* vlay = static_cast<JZVBoxLayout*>(page->layout());

        JZListWidget* lw = new JZListWidget(page);
        lw->setObjectName("demoList");
        lw->setItemHeight(26);
        vlay->addWidget(lw, 1);
        m_listDemo = lw;

        // 选中 / 激活: 列表自己就发 (点击项 = 选中 + 激活)
        connect(&lw->currentIndexChanged, this, &MainWindow::onListSelected);
        connect(&lw->sigActivated, this, &MainWindow::onListActivated);

        // 按钮行: 加一项 / 删当前项 / 清空
        JZWidget* bar = new JZWidget(page);
        JZHBoxLayout* hlay = new JZHBoxLayout(6, 6);
        bar->setLayout(hlay);

        JZPushButton* bAdd = new JZPushButton("Add Item", bar);
        JZPushButton* bDel = new JZPushButton("Remove Item", bar);
        JZPushButton* bClear = new JZPushButton("Clear", bar);
        bAdd->setObjectName("btnListAdd");
        bDel->setObjectName("btnListRemove");
        bClear->setObjectName("btnListClear");
        connect(&bAdd->sigClicked, this, &MainWindow::onListAdd);
        connect(&bDel->sigClicked, this, &MainWindow::onListRemove);
        connect(&bClear->sigClicked, this, &MainWindow::onListClear);
        hlay->addWidget(bAdd);
        hlay->addWidget(bDel);
        hlay->addWidget(bClear);
        hlay->addStretch(1);
        vlay->addWidget(bar, 0);

        appendListItems(3);
        m_preview->addWidget(page);
    }

    // ---- Table ----
    {
        JZWidget* page = makePage();
        JZVBoxLayout* vlay = static_cast<JZVBoxLayout*>(page->layout());

        JZLabel* hint = new JZLabel("点击单元格 / 方向键 选中(蓝底高亮); 选择粒度见下方下拉框", page);
        vlay->addWidget(hint, 0);

        JZTableWidget* tbl = new JZTableWidget(page);
        tbl->setColumnCount(3);
        tbl->setHorizontalHeaderLabels({ "Id", "Name", "Val" });
        vlay->addWidget(tbl, 1);
        m_table = tbl;

        // 选中变化: 点击单元格或方向键都会发 (点击 → setCurrentCell)
        connect(&tbl->currentRowChanged, this, &MainWindow::onTableRowChanged);
        connect(&tbl->currentColumnChanged, this, &MainWindow::onTableColChanged);

        // 按钮行: 增加数据 / 清空数据
        JZWidget* bar = new JZWidget(page);
        JZHBoxLayout* hlay = new JZHBoxLayout(6, 6);
        bar->setLayout(hlay);

        JZPushButton* bAdd = new JZPushButton("Add Row", bar);
        JZPushButton* bAdd10 = new JZPushButton("Add 10", bar);
        JZPushButton* bClear = new JZPushButton("Clear", bar);
        bAdd->setObjectName("btnTableAdd");
        bAdd10->setObjectName("btnTableAdd10");
        bClear->setObjectName("btnTableClear");
        connect(&bAdd->sigClicked, this, &MainWindow::onTableAdd);
        connect(&bAdd10->sigClicked, this, &MainWindow::onTableAdd10);
        connect(&bClear->sigClicked, this, &MainWindow::onTableClear);
        hlay->addWidget(bAdd);
        hlay->addWidget(bAdd10);
        hlay->addWidget(bClear);

        // 选中粒度: 按单元格(默认) / 按整行
        JZComboBox* cbSel = new JZComboBox(page);
        cbSel->setObjectName("cbTableSel");
        cbSel->addItems({ "按单元格", "按整行" });
        connect(&cbSel->currentIndexChanged, this, &MainWindow::onTableSelBehaviorChanged);
        hlay->addWidget(cbSel);

        hlay->addStretch(1);

        vlay->addWidget(bar, 0);

        // 初始演示数据
        appendTableRows(3);
        m_preview->addWidget(page);
    }

    // ---- Chart: 折线 / 柱状 / 饼图 (下拉框切换类型 + 随机数据) ----
    {
        JZWidget* page = makePage();
        JZVBoxLayout* vlay = static_cast<JZVBoxLayout*>(page->layout());

        JZLabel* hint = new JZLabel("JZChart: 下拉框切换类型; 饼图只吃第一条序列(显示百分比)", page);
        vlay->addWidget(hint, 0);

        JZChart* chart = new JZChart(page);
        chart->setObjectName("demoChart");
        chart->setTitle("月度销量");
        chart->setCategories({ "一月", "二月", "三月", "四月", "五月", "六月" });
        chart->setShowLegend(true);
        chart->setShowGrid(true);
        chart->setShowAxis(true);
        m_chart = chart;
        vlay->addWidget(chart, 1);        // 图表吃满剩余高度

        // 按钮行: 类型下拉 + 随机数据
        JZWidget* bar = new JZWidget(page);
        JZHBoxLayout* hlay = new JZHBoxLayout(6, 6);
        bar->setLayout(hlay);

        JZComboBox* cbType = new JZComboBox(bar);
        cbType->setObjectName("cbChartType");
        cbType->addItems({ "折线图", "柱状图", "饼图" });
        connect(&cbType->currentIndexChanged, this, &MainWindow::onChartTypeChanged);
        hlay->addWidget(cbType, 0);

        JZPushButton* bRand = new JZPushButton("随机数据", bar);
        bRand->setObjectName("btnChartRandom");
        connect(&bRand->sigClicked, this, &MainWindow::onChartRandomize);
        hlay->addWidget(bRand, 0);
        hlay->addStretch(1);
        vlay->addWidget(bar, 0);

        fillChartSeries(false);           // 初始数据: 两条序列 x 6 个分类
        m_preview->addWidget(page);       // Chart 页: 顺序须与 m_names 一致(TableWidget 之后 / ImageLabel 之前)
    }

    // ---- ImageLabel ----
    {
        JZWidget* page = makePage();
        JZImageLabel* img = new JZImageLabel(page);
        img->resize(60, 60);
        JZLabel* note = new JZLabel(page);
        note->setText("(no image loaded)");
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(img);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(note);
        m_preview->addWidget(page);
    }

    // ---- Dialog: JZFileDialog + JZMessageBox ----
    {
        JZWidget* page = makePage();
        JZVBoxLayout* vlay = static_cast<JZVBoxLayout*>(page->layout());

        JZLabel* title = new JZLabel(page);
        title->setText("Native file dialogs & message boxes");
        vlay->addWidget(title, 0);

        // 文件对话框按钮行
        JZWidget* fileBar = new JZWidget(page);
        JZHBoxLayout* fileLay = new JZHBoxLayout(6, 6);
        fileBar->setLayout(fileLay);

        JZPushButton* bOpen   = new JZPushButton("Open File...", fileBar);
        JZPushButton* bDir    = new JZPushButton("Choose Directory...");
        JZPushButton* bOpenM  = new JZPushButton("Open Files...", fileBar);
        JZPushButton* bSave   = new JZPushButton("Save File...", fileBar);
        connect(&bDir->sigClicked, this, &MainWindow::onFileOpen);
        connect(&bOpenM->sigClicked, this, &MainWindow::onFileOpenMany);
        connect(&bSave->sigClicked, this, &MainWindow::onFileSave);
        connect(&bDir->sigClicked, this, &MainWindow::onFileDir);
        fileLay->addWidget(bOpen);
        fileLay->addWidget(bOpenM);
        fileLay->addWidget(bSave);
        fileLay->addWidget(bDir);
        fileLay->addStretch(1);
        vlay->addWidget(fileBar, 0);

        // MessageBox 按钮行
        JZWidget* msgBar = new JZWidget(page);
        JZHBoxLayout* msgLay = new JZHBoxLayout(6, 6);
        msgBar->setLayout(msgLay);

        JZPushButton* bInfo = new JZPushButton("Information", msgBar);
        JZPushButton* bWarn = new JZPushButton("Warning", msgBar);
        JZPushButton* bQues = new JZPushButton("Question", msgBar);
        bInfo->setObjectName("btnMsgInfo");
        bWarn->setObjectName("btnMsgWarn");
        bQues->setObjectName("btnMsgQuestion");
        connect(&bInfo->sigClicked, this, &MainWindow::onMsgInfo);
        connect(&bWarn->sigClicked, this, &MainWindow::onMsgWarn);
        connect(&bQues->sigClicked, this, &MainWindow::onMsgQuestion);
        msgLay->addWidget(bInfo);
        msgLay->addWidget(bWarn);
        msgLay->addWidget(bQues);
        msgLay->addStretch(1);
        vlay->addWidget(msgBar, 0);

        // 最近一次结果
        m_fileResult = new JZLabel(page);
        m_fileResult->setText("(nothing selected yet)");
        vlay->addWidget(m_fileResult, 0);
        vlay->addStretch(1);

        m_preview->addWidget(page);
    }

    // ---- TabWidget ----
    {
        JZWidget* page = makePage();
        JZTabWidget* tabs = new JZTabWidget(page);
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(tabs, 1);

        JZWidget* tabA = new JZWidget();
        tabA->setLayout(new JZVBoxLayout(10, 10));
        JZLabel* la = new JZLabel(tabA);
        la->setText("Tab A content");
        la->setAlign(JZAlignCenter);
        static_cast<JZVBoxLayout*>(tabA->layout())->addWidget(la);

        JZWidget* tabB = new JZWidget();
        tabB->setLayout(new JZVBoxLayout(10, 10));
        JZPushButton* bb = new JZPushButton("Btn in Tab B");
        connect(&bb->sigClicked, this, &MainWindow::onLogVoid);
        static_cast<JZVBoxLayout*>(tabB->layout())->addWidget(bb);

        JZWidget* tabC = new JZWidget();
        tabC->setLayout(new JZVBoxLayout(10, 10));
        JZCheckbox* cc = new JZCheckbox(tabC);
        cc->setText("Checkbox in Tab C");
        connect(&cc->sigToggled, this, &MainWindow::onLogBool);
        static_cast<JZVBoxLayout*>(tabC->layout())->addWidget(cc);

        tabs->addTab(tabA, "A");
        tabs->addTab(tabB, "B");
        tabs->addTab(tabC, "C");
        connect(&tabs->currentChanged, this, &MainWindow::onLogInt);
        m_preview->addWidget(page);
    }

    // ---- Splitter: 经典嵌套分栏 (外层左右 + 右侧上下, 两侧分隔条都可拖) ----
    {
        JZWidget* page = makePage();
        JZVBoxLayout* vlay = static_cast<JZVBoxLayout*>(page->layout());

        // 外层横向分隔: 左(浅蓝, stretch 1) / 右(内嵌纵向 splitter, stretch 2)
        JZSplitter* hsp = new JZSplitter(JZSplitter::Horizontal, page);
        hsp->setHandleWidth(6);
        hsp->setStretchFactor(0, 1);
        hsp->setStretchFactor(1, 2);

        ColorPanel* left = new ColorPanel(JZColor(173, 216, 230), hsp);   // light blue

        // 右侧内嵌纵向分隔: 上(浅绿, stretch 2) / 下(浅黄, stretch 1),
        // 拖动横条分隔条即可在右侧区域内上下拉动调占比
        JZSplitter* right = new JZSplitter(JZSplitter::Vertical, hsp);
        right->setHandleWidth(6);
        right->setStretchFactor(0, 2);
        right->setStretchFactor(1, 1);

        ColorPanel* top = new ColorPanel(JZColor(144, 238, 144), right);    // light green
        ColorPanel* bottom = new ColorPanel(JZColor(255, 250, 205), right); // light yellow

        right->addWidget(top);
        right->addWidget(bottom);

        hsp->addWidget(left);
        hsp->addWidget(right);

        connect(&hsp->sigSplitterMoved, this, &MainWindow::onSplitterMoved);
        connect(&right->sigSplitterMoved, this, &MainWindow::onSplitterMoved);
        vlay->addWidget(hsp, 1);
        m_preview->addWidget(page);
    }

    // ---- SpinBox ----
    {
        JZWidget* page = makePage();
        JZVBoxLayout* vlay = static_cast<JZVBoxLayout*>(page->layout());
        JZSpinBox* sb = new JZSpinBox(page);
        sb->setRange(0, 100);
        sb->setValue(42);
        sb->setSuffix(" %");
        sb->resize(120, sb->height());
        connect(&sb->sigValueChanged, this, &MainWindow::onSpinValueChanged);
        vlay->addWidget(sb);
        m_preview->addWidget(page);
    }

    // ---- ScrollBar: 4000×4000 虚拟画布 (两条滚动条驱动平移/中心位置/小地图) ----
    {
        JZWidget* page = makePage();
        VirtualCanvas* canvas = new VirtualCanvas(page);   // 滚动条内嵌于画布右下角
        static_cast<JZVBoxLayout*>(page->layout())->addWidget(canvas, 1);
        m_preview->addWidget(page);
    }

    // ---- GroupBox ----
    {
        JZWidget* page = makePage();
        JZVBoxLayout* vlay = static_cast<JZVBoxLayout*>(page->layout());

        JZGroupBox* gb = new JZGroupBox("Settings", page);
        JZVBoxLayout* glay = new JZVBoxLayout(8, 8);
        gb->contentWidget()->setLayout(glay);
        JZCheckbox* cb = new JZCheckbox(gb->contentWidget());
        cb->setText("Enable feature");
        glay->addWidget(cb);
        JZSpinBox* sb = new JZSpinBox(gb->contentWidget());
        sb->setRange(0, 100);
        sb->setValue(10);
        glay->addWidget(sb);
        glay->addWidget(new JZPushButton("Button", gb->contentWidget()));

        vlay->addWidget(gb, 1);        // 组框要挂进本页布局, 否则不参与排布(页面空白)
        m_preview->addWidget(page);    // GroupBox 页: 必须与 m_names 的顺序一致(在 ScrollArea 之前)
    }

    // ---- ScrollArea: 4000×4000 世界 + 50×50 按钮 (拖滚动条/滚轮漫游) ----
    // 内容控件 setWidget 后挂到内部 viewport, 尺寸固定为世界大小; 滚动 = 滚动区
    // 把世界平移到 viewport 内 (-sx, -sy, "Apply") 并由视口裁剪, 拖滚动条/滚轮/键盘翻页
    // 均能漫游整个 4000×4000, 点任意按钮把编号 (含世界行列坐标) 打进日志。
    {
        JZWidget* page = makePage();
        JZVBoxLayout* vlay = static_cast<JZVBoxLayout*>(page->layout());

        JZScrollArea* area = new JZScrollArea(page);
        area->setObjectName("scrollArea");
        area->verticalScrollBar()->setObjectName("scrollAreaVBar");
        area->horizontalScrollBar()->setObjectName("scrollAreaHBar");
        vlay->addWidget(area, 1);

        constexpr int kWorld = 4000;           // 世界边长
        constexpr int kCell = 80;              // 每格边长 = 按钮尺寸
        constexpr int kCount = kWorld / kCell; // 每边按钮个数 (50)

        // 内容"世界": 固定超大尺寸, 滚动区据它算出滚动范围 (横/纵滚动条均出现)
        JZWidget* world = new JZWidget();
        world->setObjectName("scrollWorld");
        world->setFixedSize(kWorld, kWorld);

        int id = 0;
        for (int r = 0; r < kCount; r++) {
            for (int c = 0; c < kCount; c++) {
                const std::string num = std::to_string(id);
                // 按钮必须挂在世界(world)下: 不在对象树上的控件渲染/命中都看不到 —— 之前
                // 只 new 出来没给父控件, 于是"世界"里一个按钮都没有(滚动区看着是空的)。
                JZPushButton* b = new JZPushButton(num, world);
                b->setFixedSize(kCell, kCell);
                b->move(c * kCell, r * kCell);
                connect(&b->sigClicked, this, [id, r, c, this]() {
                    log("scroll btn #" + std::to_string(id) + " (row=" +
                        std::to_string(r) + ", col=" + std::to_string(c) + ") clicked");
                });
                id++;
            }
        }
        area->setWidget(world);
        m_preview->addWidget(page);
    }

    // ---- Animation: JZPropertyAnimation 逐帧写 x/y/w/h, 方块在 A↔B 往返 ----
    // 一个 JZAnimationGroup 并行驱动四条属性动画 (移动 x/y + 缩放 w/h),
    // 舞台是"无布局的普通控件", 方块在其上自由 move/resize 不被布局约束。
    {
        JZWidget* page = makePage();
        JZVBoxLayout* vlay = static_cast<JZVBoxLayout*>(page->layout());

        JZLabel* hint = new JZLabel(page);
        hint->setText("动画方块: JZPropertyAnimation 修改 x / y / w / h (A<->B)");
        vlay->addWidget(hint);

        JZPushButton* go = new JZPushButton("动画: 移动+缩放");
        connect(&go->sigClicked, this, &MainWindow::startAnimDemo);
        vlay->addWidget(go, 0);            // 触发按钮也要进布局(否则点不到)

        DemoStage* stage = new DemoStage(page);
        stage->setObjectName("animStage");
        vlay->addWidget(stage, 1);

        // 被动画的方块: 挂在舞台上(舞台无布局, 由 move/resize 自由定位)。
        // ⚠ 这里**不能**用 setFixedSize(): 它把 min/max 都钉在 96, 而 JZWidget::resize()
        //   按 [minimumSize, maximumSize] 钳制 ⇒ w/h 两条动画每帧都被钳回 96,
        //   表现为"方块只移动、不缩放"(与 Qt 一致: 固定尺寸控件不能再被改尺寸)。
        JZPushButton* box = new JZPushButton("finished", stage);
        box->setObjectName("animBox");
        box->resize(96, 96);       // 给初值即可, 不要约束
        box->move(24, 24);
        connect(&box->sigClicked, this, [this]() { log("anim finished"); });

        // 演示所需的三件套交给成员(否则 startAnimDemo() 直接早退, 点了没反应)
        m_animStage = stage;
        m_animBox = box;
        m_animGroup = new JZAnimationGroup(this);

        m_preview->addWidget(page);
    }

    m_preview->setCurrentIndex(0);
}

void MainWindow::showFileResult(const std::string& what, const std::string& path)
{
    if (path.empty())
        log(what + ": (cancelled)");
    else
        log(what + ": " + path);
    if (m_fileResult)
        m_fileResult->setText(what + ": " + (path.empty() ? "(cancelled)" : path));
}

void MainWindow::onFileOpen()
{
    std::string f = JZFileDialog::getOpenFileName(
        this, "Open File", "",
        "Images (*.png *.jpg *.jpeg *.bmp *.gif);;Text (*.txt);;All Files (*.*)");
    showFileResult("open file", f);
}

void MainWindow::onFileOpenMany()
{
    std::vector<std::string> fs =
        JZFileDialog::getOpenFileNames(this, "Open Files", "", "All Files (*.*)");
    std::string joined;
    for (size_t i = 0; i < fs.size(); i++) {
        if (i) joined += "; ";
        joined += fs[i];
    }
    showFileResult("open files", joined);
}

void MainWindow::onFileSave()
{
    std::string f = JZFileDialog::getSaveFileName(
        this, "Save File", "",
        "Images (*.png *.jpg *.jpeg);;All Files (*.*)");
    showFileResult("save file", f);
}

void MainWindow::onFileDir()
{
    std::string d = JZFileDialog::getExistingDirectory(this, "Choose Directory", "");
    showFileResult("choose directory", d);
}

void MainWindow::onMsgInfo()
{
    int r = JZMessageBox::information(this, "Information",
                                      "This is an informational message.")->exec();
    log(std::string("information: ") +
        (r == JZDialog::Accepted ? "Accepted" : "Rejected"));
}

void MainWindow::onMsgWarn()
{
    int r = JZMessageBox::warning(this, "Warning",
                                  "Something might be wrong here.")->exec();
    log(std::string("warning: ") +
        (r == JZDialog::Accepted ? "Accepted" : "Rejected"));
}

void MainWindow::onMsgQuestion()
{
    int r = JZMessageBox::question(this, "Question",
                                   "Do you want to continue?")->exec();
    log(std::string("question: ") +
        (r == JZDialog::Accepted ? "Accepted" : "Rejected"));
}

void MainWindow::onSplitterMoved(int pos, int handleIndex)
{
    log("splitter moved: handle=" + std::to_string(handleIndex) +
        " pos=" + std::to_string(pos));
}

void MainWindow::onSpinValueChanged(int v)
{
    log("spin value: " + std::to_string(v));
}

void MainWindow::onScrollValueChanged(int v)
{
    log("scroll value: " + std::to_string(v));
}

// ---- Animation 演示: 方块在两个锚点间往返 ----
// A = 左上角 (24,24) 96×96; B = 右下角 (贴边 24px) 176×176。
// 当前在 A(含尺寸) 就去 B, 否则回 A。四条属性动画并行同组:
//   x/y/w/h 各是一条 JZPropertyAnimation, 全部起于控件当前值。
void MainWindow::startAnimDemo()
{
    if (!m_animStage || !m_animBox || !m_animGroup)
        return;

    JZPushButton* box = m_animBox;
    JZWidget* st = m_animStage;

    const int pad = 24;
    const int wa = 96, ha = 96;                       // A: 左上
    const int wb = 176, hb = 176;                     // B: 放大
    const int xa = pad, ya = pad;
    int xb = st->width() - wb - pad;  if (xb < pad) xb = pad;
    int yb = st->height() - hb - pad; if (yb < pad) yb = pad;

    const int tol = 6;
    bool atA = abs(box->x() - xa) <= tol && abs(box->y() - ya) <= tol
        && abs(box->width() - wa) <= tol && abs(box->height() - ha) <= tol;
    const bool toB = atA;   // 停在 A -> 去 B; 在 B / 中途 -> 回 A

    const int tx = toB ? xb : xa, ty = toB ? yb : ya;
    const int tw = toB ? wb : wa, th = toB ? hb : ha;

    const int fx = box->x(), fy = box->y();
    const int fw = box->width(), fh = box->height();

    m_animGroup->stop();                              // 清上一轮动画
    m_animGroup->setDuration(1500);
    m_animGroup->addProperty(box, "x", fx, tx);
    m_animGroup->addProperty(box, "y", fy, ty);
    m_animGroup->addProperty(box, "w", fw, tw);
    m_animGroup->addProperty(box, "h", fh, th);
    m_animGroup->start();

    log(std::string("anim start: ") + (toB ? "A->B" : "B->A") +
        " x " + std::to_string(fx) + "->" + std::to_string(tx) +
        ", y " + std::to_string(fy) + "->" + std::to_string(ty) +
        ", w " + std::to_string(fw) + "->" + std::to_string(tw) +
        ", h " + std::to_string(fh) + "->" + std::to_string(th));
}

void MainWindow::appendTableRows(int n)
{
    if (!m_table || n <= 0) return;
    int r0 = m_table->rowCount();
    m_table->setRowCount(r0 + n);
    for (int i = 0; i < n; i++) {
        m_tableSeq++;
        int r = r0 + i;
        m_table->setCellValue(r, 0, std::to_string(m_tableSeq));
        m_table->setCellValue(r, 1, "item " + std::to_string(m_tableSeq));
        m_table->setCellValue(r, 2, std::to_string(m_tableSeq * 37 % 1000));
    }
    log("table rows: " + std::to_string(m_table->rowCount()));
}

void MainWindow::onTableAdd()
{
    appendTableRows(1);
}

void MainWindow::onTableAdd10()
{
    appendTableRows(10);
}

void MainWindow::onTableClear()
{
    if (!m_table) return;
    m_table->setRowCount(0);
    log("table cleared, rows: 0");
}

void MainWindow::onTableRowChanged(int row)
{
    (void)row;
    logTableSelection();
}

void MainWindow::onTableColChanged(int col)
{
    (void)col;
    logTableSelection();
}

// 选中粒度: 按单元格(默认) / 按整行
void MainWindow::onTableSelBehaviorChanged(int idx)
{
    if (!m_table) return;
    m_table->setSelectionBehavior(idx == 1 ? JZTableWidget::SelectRows
                                           : JZTableWidget::SelectItems);
    log(std::string("table selection behavior: ") + (idx == 1 ? "row" : "cell"));
}

// 选中格变化 -> 日志: (row, col) = '文本'
void MainWindow::logTableSelection()
{
    if (!m_table) return;
    const int r = m_table->currentRow();
    const int c = m_table->currentColumn();
    if (r < 0 || c < 0) {
        log("table select: (none)");
        return;
    }
    log("table select: (" + std::to_string(r) + ", " + std::to_string(c) +
        ") = '" + m_table->itemText(r, c) + "'");
}

// ==================== ListWidget 演示 ====================

void MainWindow::appendListItems(int n)
{
    if (!m_listDemo || n <= 0) return;
    for (int i = 0; i < n; i++) {
        m_listSeq++;
        m_listDemo->appendItem("item " + std::to_string(m_listSeq));
    }
    if (m_listDemo->currentIndex() < 0 && m_listDemo->count() > 0)
        m_listDemo->setCurrentIndex(0);        // 有选中项才看得出选中态
    log("list items: " + std::to_string(m_listDemo->count()));
}

void MainWindow::onListAdd()
{
    appendListItems(1);
}

void MainWindow::onListRemove()
{
    if (!m_listDemo || m_listDemo->count() <= 0) return;
    int i = m_listDemo->currentIndex();
    if (i < 0)
        i = m_listDemo->count() - 1;
    delete m_listDemo->takeItem(i);    // Qt 语义: 摘除即交出所有权, 删除由调用方做(JS 侧 removeItem 包的就是这句)

    // 摘除当前项后列表按 Qt 语义清空选择(m_current = -1, 高亮随之消失)。
    // 列表不替调用方挪选择, 这里按常见交互补选"顶上来那一项"(删到尾部就选新的末项), 让选中态连续。
    if (m_listDemo->count() > 0) {
        int next = (i < m_listDemo->count()) ? i : m_listDemo->count() - 1;
        m_listDemo->setCurrentIndex(next);
    }
    log("list remove: " + std::to_string(i) + ", items: " + std::to_string(m_listDemo->count()));
}

void MainWindow::onListClear()
{
    if (!m_listDemo) return;
    m_listDemo->clear();
    log("list cleared, items: 0");
}

void MainWindow::onListSelected(int idx)
{
    if (!m_listDemo || idx < 0) return;
    log("list select: " + std::to_string(idx) + " '" + m_listDemo->itemText(idx) + "'");
}

void MainWindow::onListActivated(int idx)
{
    if (!m_listDemo || idx < 0) return;
    log("list activated: " + std::to_string(idx) + " '" + m_listDemo->itemText(idx) + "'");
}

// ==================== Chart 演示 ====================

// 两条序列 x 6 个分类。random=false 用固定初值(便于肉眼回归/截图比对),
// random=true 用线性同余(不引 rand 的全局状态)换一批, 每次点都不同。
void MainWindow::fillChartSeries(bool random)
{
    if (!m_chart) return;
    static const double kInitA[6] = { 82, 64, 95, 47, 73, 58 };
    static const double kInitB[6] = { 55, 78, 41, 88, 62, 70 };

    std::vector<double> a(6), b(6);
    for (int i = 0; i < 6; i++) {
        if (!random) {
            a[i] = kInitA[i];
            b[i] = kInitB[i];
            continue;
        }
        m_chartSeed = m_chartSeed * 1103515245u + 12345u;
        a[i] = 20 + (int)((m_chartSeed >> 16) % 80);
        m_chartSeed = m_chartSeed * 1103515245u + 12345u;
        b[i] = 15 + (int)((m_chartSeed >> 16) % 70);
    }

    // 序列按名字重建(clearSeries + addSeries 与"新建图表"等价), 颜色取默认调色板
    m_chart->clearSeries();
    const int s1 = m_chart->addSeries("北京", JZChart::paletteColor(0));
    const int s2 = m_chart->addSeries("上海", JZChart::paletteColor(1));
    m_chart->setSeriesData(s1, a);
    m_chart->setSeriesData(s2, b);
}

void MainWindow::onChartRandomize()
{
    if (!m_chart) return;
    fillChartSeries(true);
    log("chart data: randomized");
}

// 类型切换: 饼图只使用第 1 条序列的值(扇区名取分类标签), 顺便开关"显示数值"
// (柱状图显示各柱数值; 饼图显示百分比; 折线图保持清爽)。
void MainWindow::onChartTypeChanged(int idx)
{
    if (!m_chart) return;
    JZChart::ChartType t = JZChart::LineChart;
    if (idx == 1)
        t = JZChart::BarChart;
    else if (idx == 2)
        t = JZChart::PieChart;
    m_chart->setChartType(t);
    m_chart->setShowValue(t != JZChart::LineChart);
    log(std::string("chart type: ") + (idx == 0 ? "line" : (idx == 1 ? "bar" : "pie")));
}
