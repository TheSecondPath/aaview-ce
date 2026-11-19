@echo off
rem Build isvagent.exe (test launcher for the ISV slot) for Windows CE / MIPSII with the VS2008 device compilers.
rem The toolchain lookup is tools\ce-env.bat, shared by every head-unit program.
setlocal
call "%~dp0..\..\tools\ce-env.bat"
if errorlevel 1 exit /b 1
if not exist out mkdir out

cl %CECFLAGS% /Foout\isvagent.obj isvagent.c
if errorlevel 1 exit /b 1

link %CELFLAGS% /ENTRY:WinMainCRTStartup /OUT:out\isvagent.exe ^
     out\isvagent.obj coredll.lib corelibc.lib
if errorlevel 1 exit /b 1

echo.
echo Built out\isvagent.exe
dumpbin /nologo /headers out\isvagent.exe | findstr /i /c:"machine" /c:"subsystem"
