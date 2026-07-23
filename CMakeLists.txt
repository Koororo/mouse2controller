cmake_minimum_required(VERSION 3.15)
project(mouse2controller CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

add_executable(mouse2controller
    src/main.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx9.cpp
    res/resource.rc
)

# 图标资源:icon.ico 变化时重编 resource.rc(VS generator 编译 .rc 时会自动
# 把 rc 文件所在目录加入搜索路径,故 .rc 里写 "icon.ico" 即可找到)
set_source_files_properties(res/resource.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/icon.ico")

target_include_directories(mouse2controller PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

# UTF-8 源码/执行字符集、C++ 异常模型、优化
if(MSVC)
    target_compile_options(mouse2controller PRIVATE /utf-8 /EHsc /O2 /W3)
endif()

# console 子系统(main 入口);user32(Raw Input/消息循环)、comctl32(InitCommonControlsEx)、
# gdi32、d3d9(ImGui DX9 渲染后端)
set_target_properties(mouse2controller PROPERTIES WIN32_EXECUTABLE FALSE)
target_link_libraries(mouse2controller PRIVATE user32 comctl32 gdi32 d3d9)
