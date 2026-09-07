#ifndef JZ_WIDGET_DROPDOWN_H_
#define JZ_WIDGET_DROPDOWN_H_

#include "JZWidget.h"
#include "JZSignal.h"
#include <vector>
#include <string>

class JZWindow;
class JZListWidget;

class JZComboBox : public JZWidget
{
    JZ_OBJECT_DECLARE(JZComboBox)

public:
    explicit JZComboBox(JZWidget* parent = nullptr);

    JZSize sizeHint() const override;
    JZSize minimumSizeHint() const override;

    // 添加项
    void addItem(const std::string& text);
    void addItems(const std::vector<std::string>& texts);
    void insertItem(int index, const std::string& text);

    // 移除项
    void removeItem(int index);
    void clear();

    // 当前选中
    int currentIndex() const;
    void setCurrentIndex(int index);
    std::string currentText() const;
    void setCurrentText(const std::string& text);

    // 项信息
    std::string itemText(int index) const;
    int count() const;

    // ---- AI 语义 ----
    std::string aiText() const override { return currentText(); }
    std::vector<std::string> aiItems() const override { return m_items; }
    std::vector<JZAiAction> aiActions() const override { return {{"select", "number 项索引"}}; }

    // 下拉弹层 (Popup 型 JZWindow + JZListWidget, 惰性创建)
    void showPopup();
    void hidePopup();
    bool isPopupOpen() const;   // 以弹层真实可见性为准 (任何关闭路径都正确)

jz_signals:
    JZSigInt currentIndexChanged;   // 当前选中索引变化
    JZSigString currentTextChanged;  // 当前选中文本变化

protected:
    void paintEvent(JZPainterEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void wheelEvent(JZMouseEvent* event) override;

private:
    void ensurePopup();          // 惰性创建弹层
    void selectNext(int dir);    // 滚轮循环切换并发信号

    std::vector<std::string> m_items;
    int m_current = -1;
    JZWindow* m_popup = nullptr;
    JZListWidget* m_popupList = nullptr;
};

#endif
