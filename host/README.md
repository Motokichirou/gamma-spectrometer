# gamma_host — ПО на ПК (диагностика и калибровка)

Нативное Windows-приложение (C++ / Win32 / DirectX 11 / Dear ImGui + ImPlot).
Собирается в **единый `.exe` без внешних зависимостей** (статический CRT).

Роль: компаньон к BecqMoni — связь, диагностика, в перспективе мастер
термокалибровки и конфигурация прибора. Повседневный просмотр спектров — в BecqMoni.

## Статус

**v1 — связь + диагностика** (в работе). Каркас собирается: окно, ImGui+ImPlot,
перечисление COM-портов, протокол `shproto` и обёртка COM-порта.

## Сборка

Нужен **Visual Studio Build Tools 2022** (воркфлоу «Desktop C++»: MSVC, Windows SDK,
C++ CMake tools). Из корня репозитория:

```powershell
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -G "Visual Studio 17 2022" -A x64 -S host -B host/build
& $cmake --build host/build --config Release
```

Зависимости (Dear ImGui, ImPlot) подтягиваются автоматически через CMake FetchContent
(нужен интернет на этапе configure). Результат: `host/build/Release/gamma_host.exe`.

## Структура

```
host/
├── CMakeLists.txt
└── src/
    ├── main.cpp        # окно Win32 + DX11 + цикл ImGui/ImPlot
    ├── serial_win.*    # COM-порт через Win32 API
    └── shproto.*       # протокол BecqMoni (порт из firmware/core)
```
