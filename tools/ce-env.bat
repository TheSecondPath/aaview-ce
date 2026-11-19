@echo off
rem The toolchain lookup every head-unit build.bat shares: call it after setlocal, then check errorlevel.
rem Order: TOOLROOT if set, else C:\CE\toolchain, else VS9 plus SDKROOT.
rem Sets PATH, INCLUDE and LIB for the VS2008 MIPS device compilers, and:
rem   CEDEFS    the target's defines (Windows CE, MIPSII, release)
rem   CECFLAGS  cl flags for the head-unit programs: CEDEFS, Unicode, /W3 /O1 /GS- /QMmips2
rem   CELFLAGS  link flags for a CE 5.00 MIPS image, without oldnames.lib

if not defined TOOLROOT if exist "C:\CE\toolchain\vs9" set "TOOLROOT=C:\CE\toolchain"
if defined TOOLROOT (
    set "VS9=%TOOLROOT%\vs9"
    set "SDKROOT=%TOOLROOT%\sdk"
)
if not defined VS9     set "VS9=%ProgramFiles(x86)%\Microsoft Visual Studio 9.0"
if not defined SDKROOT set "SDKROOT=%ProgramFiles(x86)%\Windows CE Tools\wce500\STANDARDSDK_500"
if not defined CPUDIR  set "CPUDIR=MIPSII"
if not defined CEVER   set "CEVER=0x500"

set "CEBIN=%VS9%\VC\ce\bin\x86_mips"
if not exist "%CEBIN%\cl.exe" (
    echo ERROR: MIPS compiler not found at "%CEBIN%\cl.exe". Set TOOLROOT, or VS9 and SDKROOT
    exit /b 1
)
if not exist "%SDKROOT%\Include\%CPUDIR%\windows.h" (
    echo ERROR: SDK headers not found at "%SDKROOT%\Include\%CPUDIR%". See docs\toolchain.md
    exit /b 1
)
echo Toolchain: %VS9%
echo SDK:       %SDKROOT% (%CPUDIR%)

set "PATH=%CEBIN%;%VS9%\VC\bin;%VS9%\Common7\IDE;%PATH%"
set "INCLUDE=%SDKROOT%\Include\%CPUDIR%;%VS9%\VC\ce\include"
set "LIB=%SDKROOT%\Lib\%CPUDIR%;%VS9%\VC\ce\lib\%CPUDIR%"

set "CEDEFS=/D UNDER_CE=%CEVER% /D _WIN32_WCE=%CEVER% /D WINCE /D _MIPS_ /D MIPS /D MIPSII /D NDEBUG"
set "CECFLAGS=/nologo /c /W3 /O1 /GS- /QMmips2 %CEDEFS% /D UNICODE /D _UNICODE"
set "CELFLAGS=/nologo /SUBSYSTEM:WINDOWSCE,5.00 /MACHINE:MIPS /NODEFAULTLIB:oldnames.lib"
exit /b 0
