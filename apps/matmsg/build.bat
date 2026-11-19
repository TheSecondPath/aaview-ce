@echo off
rem Build MatMessage.dll for Windows CE / MIPSII with the VS2008 device compilers.
rem The toolchain lookup is tools\ce-env.bat, shared by every head-unit program.
setlocal
call "%~dp0..\..\tools\ce-env.bat"
if errorlevel 1 exit /b 1
if not exist out mkdir out

cl %CECFLAGS% /Foout\matmsg.obj matmsg.c
if errorlevel 1 exit /b 1

rem MatM_CopyData is looked up by its plain name: export it undecorated.
link %CELFLAGS% /DLL /ENTRY:DllMain /EXPORT:MatM_CopyData /OUT:out\MatMessage.dll ^
     out\matmsg.obj coredll.lib corelibc.lib
if errorlevel 1 exit /b 1

echo.
echo Built out\MatMessage.dll
dumpbin /nologo /exports out\MatMessage.dll | findstr /i "MatM"
