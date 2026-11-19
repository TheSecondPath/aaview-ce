@echo off
rem Build aaview.exe for Windows CE / MIPSII with the VS2008 device compilers.
rem The toolchain lookup is tools\ce-env.bat, shared by every head-unit program.
setlocal
call "%~dp0..\..\tools\ce-env.bat"
if errorlevel 1 exit /b 1
if not exist out mkdir out

rem C++ without exceptions or RTTI.
cl %CECFLAGS% /GR- /Foout\aaview.obj aaview.cpp
if errorlevel 1 exit /b 1

link %CELFLAGS% /ENTRY:WinMainCRTStartup /OUT:out\aaview.exe ^
     out\aaview.obj coredll.lib corelibc.lib ole32.lib uuid.lib
if errorlevel 1 exit /b 1

echo.
echo Built out\aaview.exe
dumpbin /nologo /headers out\aaview.exe | findstr /i /c:"machine" /c:"subsystem"
