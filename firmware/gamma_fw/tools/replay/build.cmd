@echo off
rem Сборка replay (MSVC). build.cmd путь\к\tpl_pmt.h  — эталон, с которым гонять детектор
rem usage: out\replay.exe raw.bin out.csv [thr] [gain] [tail.bin] [ev.csv]
setlocal
set HERE=%~dp0
set SRC=%HERE%..\..\src\core
set TPL=%~1
if "%TPL%"=="" set TPL=%SRC%\tpl_pmt.h
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
if not exist "%HERE%out" mkdir "%HERE%out"
cl /nologo /O2 /utf-8 /DDET_GUARD=40u /I"%SRC%" /FI"%TPL%" "%HERE%replay.c" "%SRC%\detector.c" /Fo"%HERE%out\\" /Fe"%HERE%out\replay.exe" || exit /b 1
