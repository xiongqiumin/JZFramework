# JZFramework ESP-IDF 组件构建文件
# 由 tools/release/esp32.py 自动生成, 请勿手工修改
#
# ESP-IDF 组件 CMakeLists.txt 通过 idf_component_register() 向构建系统
# 注册本组件:
#   INCLUDE_DIRS : 头文件搜索路径(对依赖本组件的其他组件可见)
#   REQUIRES     : 本组件编译所依赖的其他 JZFramework 组件
#   SRCS         : 源文件(此处仅导出头文件, 由各组件各自编译源文件)

idf_component_register(
    INCLUDE_DIRS "include/JZCore" "include/JZAgent" "include/JZAudio" "include/JZBle" "include/JZCoroutine" "include/JZFrp" "include/JZGpio" "include/JZHttpServer" "include/JZModbus" "include/JZSerialPort" "include/JZWifi"
    REQUIRES JZCore JZAgent JZAudio JZBle JZCoroutine JZFrp JZGpio JZHttpServer JZModbus JZSerialPort JZWifi
)
