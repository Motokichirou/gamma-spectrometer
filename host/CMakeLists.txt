cmake_minimum_required(VERSION 3.20)
project(gamma_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()
# Статический CRT -> единый .exe без VC-runtime DLL (требование: без доп. ПО)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
  add_compile_options(/utf-8 /W3)
endif()

# --- Dear ImGui + ImPlot тянем из GitHub (как STM32CubeG4) ---
include(FetchContent)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.90.9
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(implot
  GIT_REPOSITORY https://github.com/epezent/implot.git
  GIT_TAG v0.16
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(implot)

# ImGui ядро + бэкенды Win32/DX11 (своего CMake у imgui нет — собираем сами)
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC
  ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)

add_library(implot STATIC
  ${implot_SOURCE_DIR}/implot.cpp
  ${implot_SOURCE_DIR}/implot_items.cpp
  ${implot_SOURCE_DIR}/implot_demo.cpp)
target_include_directories(implot PUBLIC ${implot_SOURCE_DIR})
target_link_libraries(implot PUBLIC imgui)

add_executable(gamma_host WIN32
  src/main.cpp
  src/theme.cpp
  src/device.cpp
  src/shproto.cpp
  src/serial_win.cpp)
target_link_libraries(gamma_host PRIVATE
  imgui implot d3d11 dxgi d3dcompiler dwmapi)

if(MSVC)
  # WIN32-приложение, но точка входа — обычный main()
  target_link_options(gamma_host PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

# Консольный пробник для headless-тестов стека связи (без GUI)
add_executable(gamma_probe
  src/probe.cpp
  src/shproto.cpp
  src/serial_win.cpp)
