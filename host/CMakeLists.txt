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
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
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

# Общие модули: CLI-логика + ядро связи
set(GAMMA_CORE
  src/cli.cpp
  src/device.cpp
  src/shproto.cpp
  src/serial_win.cpp)

# gammapult.exe — двойной режим. Без подкоманды → GUI; с подкомандой → CLI.
# WINDOWS subsystem (GUI без мерцания); CLI-путь цепляется к родительской консоли.
add_executable(gammapult WIN32
  src/entry.cpp
  src/main.cpp
  src/theme.cpp
  ${GAMMA_CORE})
target_link_libraries(gammapult PRIVATE
  imgui implot d3d11 dxgi d3dcompiler dwmapi)
if(MSVC)
  target_link_options(gammapult PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

# gammapult.com — консольный двойник (CONSOLE subsystem). cmd при вводе
# `gammapult …` выбирает .com → полноценный CLI (пайпы, exit-коды, cmd ждёт).
add_executable(gammapult_com
  src/entry.cpp
  ${GAMMA_CORE})
target_compile_definitions(gammapult_com PRIVATE GAMMAPULT_CONSOLE)
set_target_properties(gammapult_com PROPERTIES OUTPUT_NAME gammapult SUFFIX ".com")
