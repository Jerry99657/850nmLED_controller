# 850 nm 红外 LED 控制验证板固件（ESP32-C3-WROOM-02-N4）
cmake_minimum_required(VERSION 3.22)

# 固件版本，显示在“关于”页；不设置时会取上层 git 仓库的 describe
set(PROJECT_VER "0.4.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# 只编译 main 及其依赖的组件，缩短编译时间
idf_build_set_property(MINIMAL_BUILD ON)
project(850nm_led_controller)
