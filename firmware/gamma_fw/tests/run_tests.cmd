@echo off
rem Тесты gamma_fw на ПК (MSVC). Запуск: tests\run_tests.cmd
rem Список: имя_теста=исходники из src\core (через пробел); добавлять строки сюда.
setlocal enabledelayedexpansion
set HERE=%~dp0
set SRC=%HERE%..\src\core
set OUT=%HERE%out
if not exist "%OUT%" mkdir "%OUT%"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1

set TESTS=smoke shproto settings becq_cal
set SRC_smoke=
set SRC_shproto=shproto.c
set SRC_settings=settings.c
set SRC_becq_cal=becq_cal.c settings.c

set FAILED=
for %%T in (%TESTS%) do (
    echo === test_%%T ===
    set SRCS=
    for %%S in (!SRC_%%T!) do set SRCS=!SRCS! "%SRC%\%%S"
    cl /nologo /O2 /utf-8 /W3 /I "%SRC%" /I "%HERE%." "%HERE%test_%%T.c" !SRCS! /Fe"%OUT%\test_%%T.exe" /Fo"%OUT%\\" >"%OUT%\build_%%T.log" 2>&1
    if errorlevel 1 (
        type "%OUT%\build_%%T.log"
        set FAILED=!FAILED! %%T
    ) else (
        "%OUT%\test_%%T.exe"
        if errorlevel 1 set FAILED=!FAILED! %%T
    )
)
if defined FAILED (
    echo FAILED:%FAILED%
    exit /b 1
)
echo ALL TESTS PASSED
exit /b 0
