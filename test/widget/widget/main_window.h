#pragma once

#include "JZWindow.h"
#include "JZLabel.h"
#include "JZPushButton.h"
#include "JZSlider.h"
#include "JZProgressBar.h"
#include "JZSwitch.h"
#include "JZCheckbox.h"
#include "JZRadioButton.h"
#include "JZListWidget.h"
#include "JZComboBox.h"
#include "JZTextEdit.h"
#include "JZTableWidget.h"
#include "JZImageLabel.h"
#include "JZStackedWidget.h"
#include "JZTabWidget.h"
#include "JZSplitter.h"
#include "JZSpinBox.h"
#include "JZScrollBar.h"
#include "JZScrollArea.h"
#include "JZGroupBox.h"
#include "JZAnimationGroup.h"
#include "JZColor.h"
#include "JZPainter.h"

#include <vector>
#include <string>

// 带背景色的演示面板: 让分隔条缩放效果可见
class ColorPanel : public JZWidget
{
public:
    explicit ColorPanel(const JZColor& c, JZWidget* parent = nullptr)
        : JZWidget(parent), m_color(c) {}

    void paintEvent(JZPainterEvent* event) override {
        (void)event;
        JZPainter p(this);
        p.fillRect(rect(), m_color);
    }

    JZColor m_color;
};

// ScrollBar 演示: 4000×4000 虚拟"世界"大画布, 右下角内嵌横向/纵向两条
// JZScrollBar 驱动可视窗口平移 (滚动条是值的源头, 拖动即产生新的偏移)。
// paintEvent 按 (m_vx, m_vy) 把世界坐标平移为屏幕坐标重绘:
//   - 内容随拖动移动 (网格 + 固定世界坐标的色块, 便于观察);
//   - 屏幕中央常驻位置指示: 显示画面中心点对应的世界坐标 (随拖动变化);
//   - 右上角小地图: 全图=整个世界, 红框=当前可视窗口的位置。
// 小地图可交互 (点击即跳 / 按住拖动平移), 移动结果经 setView 同步回滚动条,
// 滚动条值变化 (拖动/点击轨道/滚轮/小地图) 全部收敛到 m_vx/m_vy 一条路径。
class VirtualCanvas : public JZWidget
{
public:
    explicit VirtualCanvas(JZWidget* parent = nullptr);

    void onHScroll(int v);   // 横向滚动条 sigValueChanged
    void onVScroll(int v);   // 纵向滚动条 sigValueChanged

    JZScrollBar* hbar() const { return m_hbar; }
    JZScrollBar* vbar() const { return m_vbar; }

protected:
    void paintEvent(JZPainterEvent* event) override;
    void resizeEvent(JZResizeEvent* event) override;
    void wheelEvent(JZMouseEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void mouseMoveEvent(JZMouseEvent* event) override;
    void mouseReleaseEvent(JZMouseEvent* event) override;

private:
    enum { kWorldW = 4000, kWorldH = 4000, kBarTh = 16 };

    // 可视内容区域 = 自身尺寸扣除右下两条滚动条占位
    int viewW() const { int w = width() - kBarTh; return w > 0 ? w : 0; }
    int viewH() const { int h = height() - kBarTh; return h > 0 ? h : 0; }

    // 小地图几何 (与 paintEvent 完全一致); 画不下 (画布太窄) 返回 false
    bool miniRect(int& ox, int& oy, int& mw, int& mh) const;
    // 当前可视窗口在小地图内的红框矩形
    void miniFrame(const int ox, const int oy, const int mw, const int mh,
                   int& fx, int& fy, int& fw, int& fh) const;
    // 把小地图上的指针位置换算成目标可视原点并应用 (dragMode: 0=中心对准, 1=抓红框保持偏移)
    void panTo(const JZPoint& localPos);
    void setView(int vx, int vy);   // clamp 到 [0, 世界-可视], 写 m_vx/vy 并同步滚动条
    bool overMini(const JZPoint& lp) const;          // 指针是否在小地图矩形内
    void updateHoverCursor(const JZPoint& lp);       // 悬停小地图 -> SizeAll, 否则取消显式光标

    JZScrollBar* m_hbar = nullptr;
    JZScrollBar* m_vbar = nullptr;
    int m_vx = 0;   // 可视窗口左上角对应的世界坐标
    int m_vy = 0;

    // 小地图拖拽状态
    bool m_dragging = false;   // 是否正在小地图上按住拖动 (已 grabMouse)
    int  m_dragMode = 0;       // 0=中心对准跟随光标, 1=按住的是红框(保持按下点偏移)
    int  m_grabDx = 0;         // dragMode=1: 按下点相对红框左上角的小地图偏移
    int  m_grabDy = 0;
};

// 挂载所有已实现控件: 左侧 JZListWidget 选择, 右侧 JZStackedWidget 预览, 底部日志
class MainWindow : public JZWindow
{
public:
    explicit MainWindow(JZWidget* parent = nullptr);
    ~MainWindow();

private:
    void setupUi();
    JZWidget* makePage();
    void buildPages();

    // 日志
    void log(const std::string& line);

    // 换肤: 整表替换 (两份 CSS 只差 :root 令牌块)
    void applyTheme(bool dark);
    void onToggleTheme();

    // 选择
    void onSelectChanged(int idx);

    // 通用信号槽
    void onLogVoid();
    void onLogInt(int v);
    void onLogBool(bool b);
    void onLogString(const std::string& s);

    // 交互演示
    void onPbInc();
    void onLogTextEdit();
    void onLabelUpdate();
    void onSplitterMoved(int pos, int handleIndex);
    void onSpinValueChanged(int v);
    void onScrollValueChanged(int v);

    // Dialog 页: 原生文件对话框 / 消息框
    void onFileOpen();
    void onFileOpenMany();
    void onFileSave();
    void onFileDir();
    void onMsgInfo();
    void onMsgWarn();
    void onMsgQuestion();
    void showFileResult(const std::string& what, const std::string& path);

    // Animation 演示: JZPropertyAnimation 修改方块 x/y/w/h, A↔B 往返
    void startAnimDemo();

    // Table 演示: 增加/清空数据
    void appendTableRows(int n);
    void onTableAdd();
    void onTableAdd10();
    void onTableClear();
    // Table 选中: 点击单元格 / 方向键 (JZTableWidget 的 currentRowChanged/currentColumnChanged)
    void onTableRowChanged(int row);
    void onTableColChanged(int col);
    void logTableSelection();
    void onTableSelBehaviorChanged(int idx);   // 选择粒度: 按单元格 / 按整行

    // ListWidget 演示: 增删项 / 清空 / 选中
    void appendListItems(int n);
    void onListAdd();
    void onListRemove();
    void onListClear();
    void onListSelected(int idx);
    void onListActivated(int idx);

    JZListWidget    *m_list = nullptr;
    JZWidget        *m_top = nullptr;
    JZStackedWidget *m_preview = nullptr;
    JZTextEdit      *m_log = nullptr;
    std::string      m_logText;
    std::vector<std::string> m_names;

    // 交互演示用到的控件引用
    JZProgressBar *m_pb = nullptr;
    int  m_pbVal = 0;
    JZTextEdit *m_te = nullptr;
    JZLabel    *m_labelDemo = nullptr;

    // Dialog 页: 显示最后一次文件对话框/消息框结果
    JZLabel *m_fileResult = nullptr;

    // Table 演示
    JZTableWidget* m_table = nullptr;
    int m_tableSeq = 0;

    // ListWidget 演示
    JZListWidget* m_listDemo = nullptr;
    int m_listSeq = 0;

    // Animation 演示
    JZAnimationGroup* m_animGroup = nullptr;   // 组并行驱动 x/y/w/h 四条动画
    JZWidget*         m_animStage = nullptr;   // 演示舞台 (网格背景, 无布局)
    JZPushButton*     m_animBox = nullptr;     // 被动画的方块 (objectName=animBox)

    // 换肤
    JZPushButton* m_themeBtn = nullptr;
    bool          m_dark = false;
};
