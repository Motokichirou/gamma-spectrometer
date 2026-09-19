@echo off
rem Build firmware/core/dsp.c + dsp_shim.c into out\dsp_shim.dll (MSVC x64) for mc.py
setlocal
set HERE=%~dp0
set OUTDIR=%HERE%out
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cl /nologo /O2 /LD /utf-8 /W3 /wd4068 /I "%HERE%..\..\firmware\core" "%HERE%dsp_shim.c" "%HERE%..\..\firmware\core\dsp.c" /Fe"%OUTDIR%/dsp_shim.dll" /Fo"%OUTDIR%/" /link /NOLOGO
