@echo off
rem Build vagent.exe for Windows CE / MIPSII with the VS2008 device compilers.
rem The toolchain lookup is tools\ce-env.bat, shared by every head-unit program.
setlocal
call "%~dp0..\..\tools\ce-env.bat"
if errorlevel 1 exit /b 1
if not exist out mkdir out

cl %CECFLAGS% /Foout\vagent.obj vagent.c
if errorlevel 1 exit /b 1

link %CELFLAGS% /ENTRY:WinMainCRTStartup /OUT:out\vagent.exe ^
     out\vagent.obj coredll.lib corelibc.lib
if errorlevel 1 exit /b 1

echo.
echo Built out\vagent.exe
dumpbin /nologo /headers out\vagent.exe | findstr /i /c:"machine" /c:"subsystem"
