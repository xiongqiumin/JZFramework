// 由 tools/ui2jz.py 从原版 StepDataDialog.ui 生成 —— **不要手改**;
// 改原 .ui 后重跑: python tools/ui2jz.py ui --out src/ui
// 保真度报告见 StepDataDialog.ui.report.txt
export const STEPDATADIALOG_UI_XML = `<?xml version="1.0" encoding="UTF-8"?>
<ui version="4.0">
  <class>StepDataDialog</class>
  <widget class="Window" name="main">
    <property name="geometry">0,0,900,600</property>
    <property name="title">DataViewDialog</property>
      <widget class="VBoxLayout" name="verticalLayout_4">
        <widget class="Widget" name="widget_4">
          <widget class="HBoxLayout" name="horizontalLayout">
            <widget class="PushButton" name="btnPlotReset">
              <property name="text">曲线复位</property>
            </widget>
            <widget class="PushButton" name="btnUpdate">
              <property name="text">刷新</property>
            </widget>
            <widget class="PushButton" name="btnUpChannel">
              <property name="text">上一个通道</property>
            </widget>
            <widget class="PushButton" name="btnDownChannel">
              <property name="text">下一个通道</property>
            </widget>
          </widget>
        </widget>
        <widget class="Splitter" name="splitter">
          <widget class="TabWidget" name="tabWidget">
            <property name="currentIndex">0</property>
            <widget class="Widget" name="tab">
              <property name="title">时间--电压&amp;电流</property>
              <widget class="VBoxLayout" name="verticalLayout_3">
                <widget class="Widget" name="widget">
                  <widget class="HBoxLayout" name="horizontalLayout_2">
                    <widget class="Label" name="label1L">
                      <property name="text"></property>
                    </widget>
                    <widget class="Label" name="label1R">
                      <property name="text"></property>
                    </widget>
                  </widget>
                </widget>
                <widget class="Chart" name="plot1">
                </widget>
              </widget>
            </widget>
            <widget class="Widget" name="tab_2">
              <property name="title">容量--电压</property>
              <widget class="VBoxLayout" name="verticalLayout">
                <widget class="Widget" name="widget_2">
                  <widget class="HBoxLayout" name="horizontalLayout_4">
                    <widget class="Label" name="label2L">
                      <property name="text"></property>
                    </widget>
                    <widget class="Label" name="label2R">
                      <property name="text"></property>
                    </widget>
                  </widget>
                </widget>
                <widget class="Chart" name="plot2">
                </widget>
              </widget>
            </widget>
            <widget class="Widget" name="tab3">
              <property name="title">时间--容量&amp;比容量</property>
              <widget class="VBoxLayout" name="verticalLayout_2">
                <widget class="Widget" name="widget_3">
                  <widget class="HBoxLayout" name="horizontalLayout_3">
                    <widget class="Label" name="label3L">
                      <property name="text"></property>
                    </widget>
                    <widget class="Label" name="label3R">
                      <property name="text"></property>
                    </widget>
                  </widget>
                </widget>
                <widget class="Chart" name="plot3">
                </widget>
              </widget>
            </widget>
          </widget>
          <widget class="TableWidget" name="treeWidget">
          </widget>
        </widget>
      </widget>
  </widget>
</ui>
`;
