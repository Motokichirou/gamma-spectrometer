@echo off
rem Прошивка gamma_stage1.elf в Nucleo-G474RE через ST-LINK (SWD)
set CLI="C:\ST\STM32CubeIDE_1.19.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.200.202503041107\tools\bin\STM32_Programmer_CLI.exe"
set ELF="C:\gamma-spectrometer\firmware\gamma_stage1\Debug\gamma_stage1.elf"
%CLI% -c port=SWD mode=UR -w %ELF% -v -rst
pause
