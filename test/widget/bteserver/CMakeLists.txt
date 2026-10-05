# test_bteserver —— C++ 版 bteserver 的**界面骨架**(见 BTEServer.h 的说明)。
#
# 文件与类的名字对齐原版 bteserver/:
#   BTEServer.{h,cpp}      <- bteserver/BTEServer/BTEServer.{h,cpp}
#   BTEItemWidget.{h,cpp}  <- bteserver/BTEServer/BTEItemWidget.{h,cpp}
#   PackDefine.{h,cpp}     <- bteserver/common/PackDefine.{h,cpp}(+ common/net/netPack 那半个, 见文件头)
#   DataTrans.{h,cpp}      <- bteserver/common/DataTrans.{h,cpp}
#   Common.{h,cpp}         <- bteserver/common/Common.{h,cpp}
#   BTERecordDataBase.{h,cpp} / BTEProject.{h,cpp} / AlarmPlayer.{h,cpp}
#                          <- bteserver/BTEServer/ 下的同名文件(记录库用引擎自带的 sqlite3)
#   HelpDialog / DeviceMacDialog <- 同名对话框(界面来自同名 .ui)
#   BTEServer.ui           <- bteserver/BTEServer/BTEServer.ui
#
# 界面源头 = BTEServer.ui(保留在源码树里); 构建期由 jz_add_ui -> tools/jz_uic 生成
# ui_BTEServer.h(namespace Ui { class BTEServerUi }; 原版是 Ui::BTEServerClass, jz_uic
# 固定按 "<.ui 文件名>Ui" 生成, 改不了), 宿主只写逻辑。
# 顺带也是一份"从 .ui 生成 C++ 代码"的样例。
#
# 该程序默认开窗口(需要桌面会话), 所以这里不注册 ctest; 自检用 test_bteserver --selftest 手动跑。
add_executable(test_bteserver
    main.cpp
    BTEServer.cpp
    BTEItemWidget.cpp
    BTERubberBand.cpp
    PackDefine.cpp
    DataTrans.cpp
    Common.cpp
    BTEProject.cpp
    BTERecordDataBase.cpp
    AlarmPlayer.cpp
    HelpDialog.cpp
    StepSettingDialog.cpp
    StepEditDialog.cpp
    DeviceMacDialog.cpp
)
target_link_libraries(test_bteserver JZFramework)

jz_add_ui(test_bteserver
    BTEServer.ui
    HelpDialog.ui
    DeviceMacDialog.ui
)
