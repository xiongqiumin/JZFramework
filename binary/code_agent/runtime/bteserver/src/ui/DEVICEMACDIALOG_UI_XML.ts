// 由 tools/ui2jz.py 从原版 DeviceMacDialog.ui 生成 —— **不要手改**;
// 改原 .ui 后重跑: python tools/ui2jz.py ui --out src/ui
// 保真度报告见 DeviceMacDialog.ui.report.txt
export const DEVICEMACDIALOG_UI_XML = `<?xml version="1.0" encoding="UTF-8"?>
<ui version="4.0">
  <class>DeviceMacDialog</class>
  <widget class="Dialog" name="main">
    <property name="geometry">0,0,320,168</property>
    <property name="title">StepEditDialog</property>
      <widget class="VBoxLayout" name="verticalLayout">
        <widget class="GroupBox" name="groupBox">
          <property name="title">参数设置</property>
          <widget class="VBoxLayout" name="gridLayout_2">
            <widget class="LineEdit" name="lineMac">
            </widget>
            <widget class="Label" name="label2">
              <property name="text">中位机号</property>
            </widget>
            <widget class="Label" name="label3">
              <property name="text">写MAC号</property>
            </widget>
            <widget class="LineEdit" name="lineDevice">
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
