cmake_minimum_required(VERSION 3.20)
project(gammapult LANGUAGES CXX)

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
  src/serial_win.cpp
  src/tpl_learn.cpp
  src/stm32boot.cpp
  src/fwflash.cpp
  src/peakfit.cpp)

# Встроенный образ прошивки gamma_fw (для прошивки голого прибора из пульта). Собирается заранее:
# firmware/gamma_fw/build.sh. Нет файла — пульт шьёт только из выбранного .bin.
set(GAMMA_FW_BIN "${CMAKE_SOURCE_DIR}/../firmware/gamma_fw/gamma_fw.bin" CACHE FILEPATH "образ прошивки для встраивания")
find_package(Git QUIET)
set(GAMMA_FW_TAG "")
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_SOURCE_DIR}/.. log -1 --format=%h_%cs -- firmware/gamma_fw/src
                  OUTPUT_VARIABLE GAMMA_FW_TAG OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/gen/fw_embed.h
  COMMAND ${CMAKE_COMMAND} -DFW_BIN=${GAMMA_FW_BIN} -DFW_TAG=gamma_fw_${GAMMA_FW_TAG} -DOUT=${CMAKE_BINARY_DIR}/gen/fw_embed.h -P ${CMAKE_SOURCE_DIR}/cmake/embed_fw.cmake
  DEPENDS ${CMAKE_SOURCE_DIR}/cmake/embed_fw.cmake ${GAMMA_FW_BIN}
  COMMENT "встраивание прошивки gamma_fw")
add_custom_target(fw_embed DEPENDS ${CMAKE_BINARY_DIR}/gen/fw_embed.h)

# gammapult.exe — двойной режим. Без подкоманды → GUI; с подкомандой → CLI.
# WINDOWS subsystem (GUI без мерцания); CLI-путь цепляется к родительской консоли.
add_executable(gammapult WIN32
  src/entry.cpp
  src/main.cpp
  src/theme.cpp
  ${GAMMA_CORE})
target_link_libraries(gammapult PRIVATE
  imgui implot d3d11 dxgi d3dcompiler dwmapi)
target_include_directories(gammapult PRIVATE ${CMAKE_BINARY_DIR}/gen)
add_dependencies(gammapult fw_embed)
if(MSVC)
  target_link_options(gammapult PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

# gammapult.com — консольный двойник (CONSOLE subsystem). cmd при вводе
# `gammapult …` выбирает .com → полноценный CLI (пайпы, exit-коды, cmd ждёт).
add_executable(gammapult_com
  src/entry.cpp
  ${GAMMA_CORE})
target_compile_definitions(gammapult_com PRIVATE GAMMAPULT_CONSOLE)
target_include_directories(gammapult_com PRIVATE ${CMAKE_BINARY_DIR}/gen)
add_dependencies(gammapult_com fw_embed)
set_target_properties(gammapult_com PROPERTIES OUTPUT_NAME gammapult SUFFIX ".com")
