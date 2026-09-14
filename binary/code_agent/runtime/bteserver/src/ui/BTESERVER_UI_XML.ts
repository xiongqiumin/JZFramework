// 由 tools/ui2jz.py 从原版 BTEServer.ui 生成 —— **不要手改**;
// 改原 .ui 后重跑: python tools/ui2jz.py ui --out src/ui
// 保真度报告见 BTEServer.ui.report.txt
export const BTESERVER_UI_XML = `<?xml version="1.0" encoding="UTF-8"?>
<ui version="4.0">
  <class>BTEServerClass</class>
  <widget class="MainWindow" name="main">
    <property name="geometry">0,0,794,757</property>
    <property name="title">BTEServer</property>
      <menubar id="menuBar">
        <menu title="文件">
          <action text="退出"/>
        </menu>
        <menu title="翻译">
          <action text="中文" checkable="true" checked="true"/>
          <action text="英语" checkable="true"/>
        </menu>
        <menu title="帮助">
          <action text="关于"/>
        </menu>
      </menubar>
      <widget class="Widget" name="centralWidget">
        <widget class="VBoxLayout" name="verticalLayout_8">
          <widget class="Splitter" name="splitter">
            <widget class="Widget" name="widget">
              <widget class="VBoxLayout" name="verticalLayout_3">
                <widget class="Label" name="label_6">
                  <property name="text">设备列表</property>
                </widget>
                <widget class="TabWidget" name="tabWidget">
                  <property name="currentIndex">1</property>
                  <widget class="Widget" name="tabDevice">
                    <property name="title">当前</property>
                    <widget class="VBoxLayout" name="verticalLayout_2">
                      <widget class="ListWidget" name="listWidget">
                      </widget>
                    </widget>
                  </widget>
                  <widget class="Widget" name="tabQuery">
                    <property name="title">历史数据</property>
                    <widget class="VBoxLayout" name="verticalLayout_10">
                      <widget class="Label" name="label_7">
                        <property name="text">查询</property>
                      </widget>
                      <widget class="GroupBox" name="groupBox">
                        <property name="title">设备</property>
                        <widget class="VBoxLayout" name="gridLayout_2">
                          <widget class="Label" name="label_8">
                            <property name="text">设备号</property>
                          </widget>
                          <widget class="ComboBox" name="boxDevice">
                          </widget>
                          <widget class="Label" name="label_9">
                            <property name="text">单元号</property>
                          </widget>
                          <widget class="Label" name="label_10">
                            <property name="text">通道号</property>
                          </widget>
                          <widget class="LineEdit" name="lineQueryUnit">
                          </widget>
                          <widget class="LineEdit" name="lineQueryChannel">
                          </widget>
                        </widget>
                      </widget>
                      <widget class="GroupBox" name="groupBox_2">
                        <property name="title">时间</property>
                        <widget class="VBoxLayout" name="verticalLayout_11">
                          <widget class="RadioButton" name="btnNone">
                            <property name="text">不记得</property>
                            <property name="checked">true</property>
                          </widget>
                          <widget class="Widget" name="widget_12">
                            <widget class="HBoxLayout" name="horizontalLayout_8">
                              <widget class="RadioButton" name="btnNear">
                                <property name="text">最近</property>
                              </widget>
                              <widget class="SpinBox" name="lineNearDay">
                                <property name="maximum">1000</property>
                              </widget>
                            </widget>
                          </widget>
                          <widget class="Widget" name="widget_2">
                            <widget class="HBoxLayout" name="horizontalLayout_7">
                              <widget class="RadioButton" name="btnFrom">
                                <property name="text">从</property>
                              </widget>
                              <widget class="LineEdit" name="dateFrom">
                              </widget>
                            </widget>
                          </widget>
                          <widget class="Widget" name="widget_11">
                            <widget class="HBoxLayout" name="horizontalLayout_2">
                              <widget class="Label" name="label_14">
                                <property name="text">至</property>
                              </widget>
                              <widget class="LineEdit" name="dateTo">
                              </widget>
                            </widget>
                          </widget>
                        </widget>
                      </widget>
                      <widget class="Label" name="label_11">
                        <property name="text">批号/电池型号</property>
                      </widget>
                      <widget class="LineEdit" name="lineCellPatch">
                      </widget>
                      <widget class="Label" name="label_12">
                        <property name="text">创建者</property>
                      </widget>
                      <widget class="LineEdit" name="lineCreator">
                      </widget>
                      <widget class="Label" name="label_13">
                        <property name="text">电池编号</property>
                      </widget>
                      <widget class="LineEdit" name="lineCellNum">
                      </widget>
                      <widget class="PushButton" name="btnQuery">
                        <property name="text">查询</property>
                      </widget>
                      <widget class="PushButton" name="btnExportAll">
                        <property name="text">导出当前数据</property>
                      </widget>
                      <widget class="PushButton" name="btnDeleteSelect">
                        <property name="text">删除当前数据</property>
                      </widget>
                      <widget class="PushButton" name="btnDeleteWeek">
                        <property name="text">删除一周前数据</property>
                      </widget>
                      <widget class="PushButton" name="btnDeleteMonth">
                        <property name="text">删除一月前数据</property>
                      </widget>
                      <widget class="PushButton" name="btnDeleteYear">
                        <property name="text">删除一年前数据</property>
                      </widget>
                      <widget class="PushButton" name="btnDeleteDevice">
                        <property name="text">删除当前设备</property>
                      </widget>
                    </widget>
                  </widget>
                </widget>
              </widget>
            </widget>
            <widget class="Widget" name="widgetMain">
              <widget class="VBoxLayout" name="verticalLayout">
                <widget class="Label" name="label_15">
                  <property name="text">通道列表</property>
                </widget>
                <widget class="StackedWidget" name="stackedWidget">
                  <property name="currentIndex">3</property>
                  <widget class="Widget" name="pageNormal">
                    <widget class="VBoxLayout" name="verticalLayout_7">
                      <widget class="ScrollArea" name="scrollArea">
                        <widget class="Widget" name="itemWidgets">
                          <property name="geometry">0,0,98,28</property>
                        </widget>
                      </widget>
                    </widget>
                  </widget>
                  <widget class="Widget" name="pageList">
                    <widget class="VBoxLayout" name="verticalLayout_12">
                      <widget class="TableWidget" name="tableList">
                      </widget>
                    </widget>
                  </widget>
                  <widget class="Widget" name="pageHistory">
                    <widget class="VBoxLayout" name="verticalLayout_9">
                      <widget class="TableWidget" name="tableQuery">
                      </widget>
                    </widget>
                  </widget>
                  <widget class="Widget" name="pageSplit">
                    <widget class="VBoxLayout" name="verticalLayout_14">
                      <widget class="TabWidget" name="tabWidgetSplit">
                        <property name="currentIndex">0</property>
                        <widget class="Widget" name="tabSplitSetting">
                          <property name="title">分选条件设置</property>
                          <widget class="HBoxLayout" name="horizontalLayout_11">
                            <widget class="Widget" name="splitSettingWidget"></widget>
                          </widget>
                        </widget>
                        <widget class="Widget" name="tabSplitResult">
                          <property name="title">分选结果</property>
                          <widget class="VBoxLayout" name="verticalLayout_19">
                            <widget class="Widget" name="widget_20">
                              <widget class="HBoxLayout" name="horizontalLayout_15">
                                <widget class="Widget" name="splitResult"></widget>
                                <widget class="Chart" name="splitPie">
                                </widget>
                              </widget>
                            </widget>
                            <widget class="GroupBox" name="groupBox_5">
                              <property name="title">结果列表显示</property>
                              <widget class="HBoxLayout" name="horizontalLayout_14">
                                <widget class="TableWidget" name="tableSplitLevel">
                                </widget>
                                <widget class="TableWidget" name="tableSplitList">
                                </widget>
                              </widget>
                            </widget>
                            <widget class="Widget" name="widget_13">
                              <widget class="HBoxLayout" name="horizontalLayout_10">
                                <widget class="PushButton" name="btnCopySplitList">
                                  <property name="text">导出分选结果</property>
                                </widget>
                              </widget>
                            </widget>
                          </widget>
                        </widget>
                      </widget>
                    </widget>
                  </widget>
                  <widget class="Widget" name="pageTest">
                    <widget class="VBoxLayout" name="verticalLayout_6">
                      <widget class="Widget" name="widget_4">
                        <widget class="HBoxLayout" name="horizontalLayout_3">
                          <widget class="Label" name="label">
                            <property name="text">端口</property>
                          </widget>
                          <widget class="LineEdit" name="linePort">
                            <property name="text">8002</property>
                          </widget>
                          <widget class="PushButton" name="btnResetServer">
                            <property name="text">重启端口</property>
                          </widget>
                          <widget class="CheckBox" name="boxRecordPack">
                            <property name="text">记录数据</property>
                          </widget>
                        </widget>
                      </widget>
                      <widget class="Widget" name="widget_10">
                        <widget class="HBoxLayout" name="horizontalLayout_9">
                          <widget class="Label" name="label_16">
                            <property name="text">数据过滤:</property>
                          </widget>
                          <widget class="CheckBox" name="boxShowDF">
                            <property name="text">不显示DF</property>
                            <property name="checked">true</property>
                          </widget>
                          <widget class="CheckBox" name="boxShowRT">
                            <property name="text">不显示RT</property>
                            <property name="checked">true</property>
                          </widget>
                        </widget>
                      </widget>
                      <widget class="Widget" name="widget_9">
                        <widget class="HBoxLayout" name="horizontalLayout_6">
                          <widget class="Widget" name="widget_5">
                            <widget class="VBoxLayout" name="verticalLayout_5">
                              <widget class="Widget" name="widget_6">
                                <widget class="HBoxLayout" name="horizontalLayout_5">
                                  <widget class="Label" name="label_3">
                                    <property name="text">send</property>
                                  </widget>
                                  <widget class="PushButton" name="btnClearSend">
                                    <property name="text">清除发送</property>
                                  </widget>
                                </widget>
                              </widget>
                              <widget class="TextEdit" name="textSend">
                              </widget>
                            </widget>
                          </widget>
                          <widget class="Widget" name="widget_7">
                            <widget class="VBoxLayout" name="verticalLayout_4">
                              <widget class="Widget" name="widget_8">
                                <widget class="HBoxLayout" name="horizontalLayout_4">
                                  <widget class="Label" name="label_4">
                                    <property name="text">recv</property>
                                  </widget>
                                  <widget class="PushButton" name="btnClearRecv">
                                    <property name="text">清除接收</property>
                                  </widget>
                                </widget>
                              </widget>
                              <widget class="TextEdit" name="textRecv">
                              </widget>
                            </widget>
                          </widget>
                        </widget>
                      </widget>
                      <widget class="Label" name="label_2">
                        <property name="text">Recv Log:</property>
                      </widget>
                      <widget class="TextEdit" name="textLog">
                      </widget>
                    </widget>
                  </widget>
                </widget>
                <widget class="Widget" name="widget_3">
                  <widget class="HBoxLayout" name="horizontalLayout">
                    <widget class="Label" name="label_5">
                      <property name="text">欢迎使用软件</property>
                    </widget>
                    <widget class="PushButton" name="btnShowNormal">
                      <property name="text">常规显示</property>
                    </widget>
                    <widget class="PushButton" name="btnShowList">
                      <property name="text">列表显示</property>
                    </widget>
                    <widget class="PushButton" name="btnShowSplitSetting">
                      <property name="text">分选设置</property>
                    </widget>
                    <widget class="PushButton" name="btnShowTest">
                      <property name="text">测试页面</property>
                    </widget>
                    <widget class="PushButton" name="btnSplitQuery">
                      <property name="text">分选</property>
                    </widget>
                    <widget class="PushButton" name="btnClearAlarm">
                      <property name="text">清除报警</property>
                    </widget>
                  </widget>
                </widget>
              </widget>
            </widget>
          </widget>
        </widget>
      </widget>
      <widget class="Widget" name="mainToolBar">
      </widget>
  </widget>
</ui>
`;
