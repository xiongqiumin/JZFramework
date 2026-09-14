// 由 tools/ui2jz.py 从原版 StepEditDialog.ui 生成 —— **不要手改**;
// 改原 .ui 后重跑: python tools/ui2jz.py ui --out src/ui
// 保真度报告见 StepEditDialog.ui.report.txt
export const STEPEDITDIALOG_UI_XML = `<?xml version="1.0" encoding="UTF-8"?>
<ui version="4.0">
  <class>StepEditDialog</class>
  <widget class="Dialog" name="main">
    <property name="geometry">0,0,439,485</property>
    <property name="title">StepEditDialog</property>
      <widget class="VBoxLayout" name="verticalLayout">
        <widget class="Widget" name="widget_2">
          <widget class="VBoxLayout" name="gridLayout">
            <widget class="Label" name="label">
              <property name="text">工步号</property>
            </widget>
            <widget class="LineEdit" name="lineNum">
              <property name="readOnly">true</property>
            </widget>
            <widget class="Label" name="label_11">
              <property name="text">工步类型</property>
            </widget>
            <widget class="ComboBox" name="boxType">
            </widget>
          </widget>
        </widget>
        <widget class="GroupBox" name="groupBox">
          <property name="title">参数设置</property>
          <widget class="VBoxLayout" name="gridLayout_2">
            <widget class="Label" name="label9">
              <property name="text">温度(℃)</property>
            </widget>
            <widget class="SpinBox" name="spinTempature">
              <property name="minimum">0</property>
              <property name="maximum">300</property>
            </widget>
            <widget class="Label" name="label3">
              <property name="text">电流(A)</property>
            </widget>
            <widget class="Label" name="label8">
              <property name="text">容量(Ah)</property>
            </widget>
            <widget class="CheckBox" name="boxSetTime">
              <property name="text">设置时间</property>
            </widget>
            <widget class="LineEdit" name="lineCurrent">
            </widget>
            <widget class="LineEdit" name="lineStopCurrent">
            </widget>
            <widget class="Label" name="label5">
              <property name="text">截止电压(V)</property>
            </widget>
            <widget class="SpinBox" name="lineStartStep">
              <property name="minimum">1</property>
              <property name="maximum">255</property>
            </widget>
            <widget class="SpinBox" name="lineLoopTime">
              <property name="minimum">1</property>
              <property name="maximum">10000</property>
            </widget>
            <widget class="LineEdit" name="lineTime">
            </widget>
            <widget class="Label" name="label6">
              <property name="text">起始工步</property>
            </widget>
            <widget class="Label" name="label7">
              <property name="text">循环次数</property>
            </widget>
            <widget class="LineEdit" name="lineStopVoltage">
            </widget>
            <widget class="LineEdit" name="lineCapacity">
              <property name="placeholderText">0.01-10000.00</property>
            </widget>
            <widget class="Label" name="label2">
              <property name="text">电压(V)</property>
            </widget>
            <widget class="Label" name="label4">
              <property name="text">截止电流(A)</property>
            </widget>
            <widget class="LineEdit" name="lineVolt">
            </widget>
          </widget>
        </widget>
        <widget class="Widget" name="widget">
          <widget class="HBoxLayout" name="horizontalLayout">
            <widget class="PushButton" name="btnOk">
              <property name="text">确定</property>
            </widget>
            <widget class="PushButton" name="btnCancel">
              <property name="text">取消</property>
            </widget>
          </widget>
        </widget>
      </widget>
  </widget>
</ui>
`;
