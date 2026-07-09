#include <stdio.h>
#include "JZGuiApplication.h"
#include "JZWidget.h"
#include "JZLayout.h"
#include "JZPushButton.h"
#include "JZLabel.h"
#include "JZCheckbox.h"
#include "JZRadioButton.h"
#include "JZSwitch.h"
#include "JZSlider.h"
#include "JZComboBox.h"
#include "JZTextEdit.h"
#include "JZRoller.h"
#include "JZLedWidget.h"
#include "JZProgressBar.h"
#include "JZArcWidget.h"
#include "JZTableWidget.h"
#include "JZColor.h"

#define W 800
#define H 600

class TestWindow : public JZWidget
{
public:
    TestWindow() : JZWidget() {
        resize(W, H);
        //setTitle("JZFramework Widgets Demo");

        // Tab bar
        tabBar = new JZWidget(this);
        tabBar->resize(W, 35);
        tabBar->move(0, 35);
        tabBar->setLayout(new JZHBoxLayout(2, 2));

        // Pages
        pages = new JZWidget(this);
        pages->resize(W, H - 75);
        pages->move(0, 72);

        tabs.resize(4);
        tabs[0].btn = makeTabBtn("Buttons", 0);
        tabs[0].container = new JZWidget(pages); tabs[0].container->resize(W, H - 75);
        createButtonsPage();

        tabs[1].btn = makeTabBtn("Inputs", 1);
        tabs[1].container = new JZWidget(pages); tabs[1].container->resize(W, H - 75);
        createInputsPage();

        tabs[2].btn = makeTabBtn("Arc&Table", 2);
        tabs[2].container = new JZWidget(pages); tabs[2].container->resize(W, H - 75);
        createArcTablePage();

        tabs[3].btn = makeTabBtn("Layout", 3);
        tabs[3].container = new JZWidget(pages); tabs[3].container->resize(W, H - 75);
        createLayoutPage();

        showTab(0);
    }

private:
    struct Tab { JZWidget* container = nullptr; JZPushButton* btn = nullptr; };
    std::vector<Tab> tabs;
    JZWidget* tabBar = nullptr;
    JZWidget* pages = nullptr;

    JZPushButton* makeTabBtn(const char* text, int idx) {
        auto* b = new JZPushButton(tabBar);
        b->setText(text);
        b->resize(100, 30);
        connect(&b->sigClicked, this, [this, idx] { showTab(idx); });
        return b;
    }

    void showTab(int idx) {
        for (int i = 0; i < (int)tabs.size(); ++i)
            tabs[i].container->setVisible(i == idx);
    }

    // === Pages ===

    void createButtonsPage() {
        auto* p = tabs[0].container;
        auto* info = new JZLabel(p);
        info->move(20, 10); info->resize(560, 25);
        info->setText("Buttons & Toggles");

        auto* btn = new JZPushButton(p);
        btn->resize(120, 36); btn->move(20, 50); btn->setText("Click Me");
        connect(&btn->sigClicked, [info] { info->setText("Button clicked!"); });

        auto* cb = new JZCheckbox(p);
        cb->move(20, 100); cb->setText("I agree");
        connect(&cb->sigToggled, [info](bool v) { info->setText(v ? "Checkbox: checked" : "Checkbox: unchecked"); });

        auto* radioGrp = new JZWidget(p);
        radioGrp->resize(200, 100); radioGrp->move(160, 50);
        auto* r1 = new JZRadioButton(radioGrp, "Option A"); r1->move(0, 0);
        auto* r2 = new JZRadioButton(radioGrp, "Option B"); r2->move(0, 30);
        auto* r3 = new JZRadioButton(radioGrp, "Option C"); r3->move(0, 60);
        r1->setChecked(true);
        connect(&r1->sigToggled, [info] { info->setText("Radio: A"); });
        connect(&r2->sigToggled, [info] { info->setText("Radio: B"); });
        connect(&r3->sigToggled, [info] { info->setText("Radio: C"); });

        auto* sw = new JZSwitch(p);
        sw->move(20, 150);
        connect(&sw->sigToggled, [info](bool v) { info->setText(v ? "Switch: ON" : "Switch: OFF"); });

        auto* led = new JZLed(p);
        led->resize(30, 30); led->move(20, 200); led->setColor(JZColor::Red());
        auto* ledBtn = new JZPushButton(p);
        ledBtn->resize(80, 30); ledBtn->move(70, 200); ledBtn->setText("Toggle LED");
        connect(&ledBtn->sigClicked, [led] { static bool on = false; on = !on; on ? led->on() : led->off(); });
    }

    void createInputsPage() {
        auto* p = tabs[1].container;
        auto* info = new JZLabel(p);
        info->move(20, 10); info->resize(560, 25); info->setText("Inputs");

        auto* slider = new JZSlider(p);
        slider->resize(200, 20); slider->move(20, 50);
        slider->setRange(0, 100); slider->setValue(50);
        auto* sVal = new JZLabel(p);
        sVal->move(240, 50); sVal->resize(60, 20); sVal->setText("50");
        connect(&slider->sigValueChanged, [sVal](int v) { sVal->setText(std::to_string(v)); });

        auto* combo = new JZComboBox(p);
        combo->move(20, 90);
        combo->addItem("Apple"); combo->addItem("Banana"); combo->addItem("Cherry"); combo->addItem("Date");
        auto* cVal = new JZLabel(p);
        cVal->move(180, 90); cVal->resize(120, 20); cVal->setText("Apple");
        connect(&combo->currentIndexChanged, [combo, cVal](int) { cVal->setText(combo->currentText()); });

        auto* textEdit = new JZTextEdit(p);
        textEdit->resize(250, 80); textEdit->move(20, 140); textEdit->setText("Edit me...");

        auto* roller = new JZRoller(p);
        roller->move(300, 50);
        roller->setOptions("Mon\nTue\nWed\nThu\nFri\nSat\nSun");
        roller->setVisibleRowCount(3);

        auto* bar = new JZProgressBar(p);
        bar->resize(200, 15); bar->move(20, 240);
        bar->setRange(0, 100); bar->setValue(30);
        auto* bVal = new JZLabel(p);
        bVal->move(240, 240); bVal->setText("30%");
        auto* barBtn = new JZPushButton(p);
        barBtn->resize(60, 25); barBtn->move(20, 265); barBtn->setText("+10%");
        connect(&barBtn->sigClicked, [bar, bVal] {
            int v = bar->value() + 10; if (v > 100) v = 0;
            bar->setValue(v, true);
            bVal->setText(std::to_string(v) + "%");
        });
    }

    void createArcTablePage() {
        auto* p = tabs[2].container;
        auto* info = new JZLabel(p);
        info->move(20, 10); info->resize(560, 25); info->setText("Arc & Table");

        auto* arc = new JZArcWidget(p);
        arc->resize(120, 120); arc->move(20, 40);
        arc->setRange(0, 100); arc->setValue(40);
        auto* aVal = new JZLabel(p);
        aVal->move(50, 170); aVal->resize(80, 20);
        aVal->setAlign(LV_TEXT_ALIGN_CENTER); aVal->setText("40");
        connect(&arc->sigValueChanged, [aVal](int v) { aVal->setText(std::to_string(v)); });

        auto* table = new JZTable(p);
        table->move(180, 40);
        table->setSize(4, 4);
        table->setColWidth(0, 80); table->setColWidth(1, 80); table->setColWidth(2, 80); table->setColWidth(3, 80);
        table->setCellValue(0, 0, "Name");    table->setCellValue(0, 1, "Score"); table->setCellValue(0, 2, "Rank"); table->setCellValue(0, 3, "Status");
        table->setCellValue(1, 0, "Alice");   table->setCellValue(1, 1, "95");   table->setCellValue(1, 2, "#1");  table->setCellValue(1, 3, "Pass");
        table->setCellValue(2, 0, "Bob");     table->setCellValue(2, 1, "87");   table->setCellValue(2, 2, "#2");  table->setCellValue(2, 3, "Pass");
        table->setCellValue(3, 0, "Charlie"); table->setCellValue(3, 1, "72");   table->setCellValue(3, 2, "#3");  table->setCellValue(3, 3, "Pass");
    }

    void createLayoutPage() {
        auto* p = tabs[3].container;
        auto* info = new JZLabel(p);
        info->move(20, 10); info->resize(560, 25); info->setText("Layouts");

        auto* vbox = new JZWidget(p);
        vbox->resize(150, 200); vbox->move(20, 40);
        vbox->setLayout(new JZVBoxLayout(4, 4));
        for (int i = 1; i <= 4; ++i)
            (new JZPushButton(vbox))->setText(("V" + std::to_string(i)).c_str());

        auto* hbox = new JZWidget(p);
        hbox->resize(400, 40); hbox->move(190, 40);
        hbox->setLayout(new JZHBoxLayout(4, 4));
        for (int i = 1; i <= 4; ++i) {
            auto* b = new JZPushButton(hbox);
            b->resize(80, 30); b->setText(("H" + std::to_string(i)).c_str());
        }

        auto* grid = new JZWidget(p);
        grid->resize(320, 150); grid->move(190, 90);
        auto* gridLay = new JZGridLayout(4, 4);
        for (int c = 0; c < 3; ++c) gridLay->setColumnMinimumWidth(c, 100);
        for (int r = 0; r < 3; ++r) gridLay->setRowMinimumHeight(r, 40);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                char buf[8]; snprintf(buf, 8, "%d,%d", r, c);
                auto* btn = new JZPushButton(grid);
                btn->setText(buf);
                gridLay->addWidget(btn, r, c);
            }
        grid->setLayout(gridLay);
    }
};

int main() {
    printf("JZFramework Widgets Demo starting...\n");

    JZGUIApplication app;
    TestWindow w;
    w.show();

    return app.exec();
}
