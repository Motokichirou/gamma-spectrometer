# gammapult — сервисное ПО на ПК (диагностика и калибровка)

Нативное Windows-приложение (C++ / Win32 / DirectX 11 / Dear ImGui + ImPlot).
Собирается в **единый исполняемый файл без внешних зависимостей** (статический CRT —
не нужны ни Python, ни Java, ни рантаймы).

Роль: сервисный компаньон к [BecqMoni](https://github.com/Am6er/BecqMoni) — связь,
диагностика, живой спектр, мастер энергокалибровки и конфигурация прибора. Повседневный
просмотр и сбор спектров можно вести в BecqMoni; калибровка `gammapult` совместима с его
кнопками «Записать/Читать с устройства» (см. [протокол](../docs/ru/protocol.md)).

> Сервисный тул свободен: он не обязан быть совместим с чужими форматами. Протокол
> shproto он не меняет — пользуется собственными командами уровня приложения.

## Двойной режим: один инструмент, два файла

Сборка даёт два файла с общим именем:

| Файл | Подсистема | Когда запускается |
|---|---|---|
| `gammapult.exe` | GUI (Windows) | двойной клик / без аргументов → графический интерфейс |
| `gammapult.com` | консоль | из терминала с подкомандой → CLI для автоматизации |

В консоли Windows при вводе `gammapult …` приоритет имеет `.com` (правило PATHEXT),
поэтому `gammapult info --port COM6` уходит в CLI, а двойной клик по `.exe` открывает окно.

## GUI (`gammapult.exe`)

Раскладка из раздвижных панелей:

- **Соединение** — выбор COM-порта, подключение, статус, серийник, версия прошивки.
- **Телеметрия** — elapsed / cps / invalid, температура, текущий порог DSP.
- **Калибровка энергий** — точки (канал↔кэВ), пресеты изотопов (включая Ra-226),
  полином 1..4 степени, фит МНК, проверка валидности по образцу BecqMoni
  (монотонность + границы), запись в прибор (`-wcal`).
- **Спектр** — живой график (ImPlot): Лин/Лог, колесо = масштаб X, Shift+колесо = Y,
  оси не уезжают за `[0..8192]` и ниже нуля, автоподгон Y.
- **Консоль** — лог обмена + чипы быстрых команд + произвольная команда.
- **Самотест** — прогон ENC (`-tst enc`), таблица центроид/σ/FWHM + графики.
- **Справка** — встроенная инструкция.

## CLI (`gammapult.com`) — для автоматизации

```
gammapult list                                  # перечислить COM-порты
gammapult info    --port COM6                   # версия прошивки + серийник
gammapult acquire --port COM6 --seconds N \     # набор спектра
                  [--gen spec|ra|mono|off] [--out file.csv]
gammapult enc     --port COM6                   # отчёт самотеста ENC
gammapult cmd     --port COM6 --send "-inf" [--read СЕК]   # произвольная команда
```

`acquire --out` пишет CSV `channel,count`; `--gen` включает тест-генератор на время
набора и возвращает его в `off` по завершении.

## Сборка

Нужен **Visual Studio Build Tools 2022** (воркфлоу «Desktop C++»: MSVC, Windows SDK,
C++ CMake tools). Из корня репозитория:

```powershell
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -G "Visual Studio 17 2022" -A x64 -S host -B host/build   # configure (1 раз)
& $cmake --build host/build --config Release
```

Зависимости (Dear ImGui v1.90.9, ImPlot v0.16) подтягиваются автоматически через CMake
FetchContent (на этапе configure нужен интернет). Результат:
`host/build/Release/gammapult.exe` и `gammapult.com`.

## Структура

```
host/
├── CMakeLists.txt      # цели gammapult (GUI) и gammapult_com (CLI, SUFFIX .com)
└── src/
    ├── entry.cpp       # диспетчер режимов GUI/CLI
    ├── main.cpp        # окно Win32 + DX11 + цикл ImGui/ImPlot (GUI)
    ├── cli.cpp         # подкоманды CLI поверх класса Device
    ├── device.*        # сессия с прибором: connect/poll/команды, разбор кадров
    ├── serial_win.*    # COM-порт через Win32 API
    ├── shproto.*       # протокол BecqMoni (порт из firmware/core)
    └── theme.*         # тема и шрифты ImGui (латиница/греческий/кириллица)
```
