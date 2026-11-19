<#
.SYNOPSIS
    Copy the pieces of a VS2008 install and a CE MIPS SDK that build.bat needs into a portable folder.

.DESCRIPTION
    Run on a machine (or throwaway VM) where VS2008 with "Visual C++ for Devices" and the MIPS SDK are installed.
    The result works on any Windows PC without VS2008 installed: copy the folder over and point TOOLROOT at it
    (build.bat also finds C:\CE\toolchain automatically).

    Layout produced:
        <Dest>\vs9\VC\ce\bin\x86_mips\   MIPS compiler + linker (with 1033\ resources)
        <Dest>\vs9\VC\ce\include\        device CRT headers
        <Dest>\vs9\VC\ce\lib\<cpu>\      device CRT libs
        <Dest>\vs9\VC\bin\               host tools (dumpbin, link) + 1033\ resources
        <Dest>\vs9\Common7\IDE\          mspdb80/msobj80 DLLs the tools load
        <Dest>\sdk\Include\<cpu>\        SDK headers
        <Dest>\sdk\Lib\<cpu>\            SDK libs (coredll.lib, ...)
        <Dest>\TOOLCHAIN.txt             where it all came from

    This is your licensed copy of VS2008; don't publish the folder.

.EXAMPLE
    .\pack-toolchain.ps1
.EXAMPLE
    .\pack-toolchain.ps1 -SdkRoot C:\CE\sdk500 -Dest D:\CE\toolchain -Force
#>
param(
    [string]$VsRoot  = "${env:ProgramFiles(x86)}\Microsoft Visual Studio 9.0",
    [string]$SdkRoot = "${env:ProgramFiles(x86)}\Windows CE Tools\wce500\STANDARDSDK_500",
    [string]$Cpu     = 'MIPSII',
    [string]$Dest    = 'C:\CE\toolchain',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

function Require([string]$Path, [string]$Hint) {
    if (-not (Test-Path $Path)) { throw "Missing: $Path`n  $Hint" }
}

function CopyDir([string]$From, [string]$To) {
    New-Item -ItemType Directory -Force $To | Out-Null
    Copy-Item -Path (Join-Path $From '*') -Destination $To -Recurse -Force
}

function CopyFiles([string]$From, [string]$To, [string[]]$Names) {
    New-Item -ItemType Directory -Force $To | Out-Null
    foreach ($n in $Names) {
        $src = Join-Path $From $n
        if (Test-Path $src) { Copy-Item $src $To -Force }
        else { Write-Warning "not found (may be fine): $src" }
    }
}

# --- check sources before touching the destination
Require "$VsRoot\VC\ce\bin\x86_mips\cl.exe" 'Install VS2008 with Smart Device Programmability > Visual C++ for Devices, or pass -VsRoot.'
Require "$VsRoot\VC\ce\include"             'VS2008 device C++ headers are missing.'
Require "$VsRoot\VC\ce\lib\$Cpu"            "No device CRT libs for $Cpu. Check -Cpu."
Require "$VsRoot\VC\bin\dumpbin.exe"        'VS2008 x86 C++ tools are missing.'
Require "$SdkRoot\Include\$Cpu\windows.h"   'Pass -SdkRoot: the folder that contains Include\<cpu>\windows.h.'
Require "$SdkRoot\Lib\$Cpu\coredll.lib"     "SDK has no Lib\$Cpu\coredll.lib."

if ((Test-Path $Dest) -and -not $Force) {
    throw "$Dest already exists. Use -Force to overwrite."
}

$vs = Join-Path $Dest 'vs9'
Write-Host "Packing toolchain into $Dest"

# --- VS2008 device compiler, CRT, host tools
CopyDir "$VsRoot\VC\ce\bin\x86_mips" "$vs\VC\ce\bin\x86_mips"
CopyDir "$VsRoot\VC\ce\include"      "$vs\VC\ce\include"
CopyDir "$VsRoot\VC\ce\lib\$Cpu"     "$vs\VC\ce\lib\$Cpu"

New-Item -ItemType Directory -Force "$vs\VC\bin" | Out-Null
Get-ChildItem "$VsRoot\VC\bin" -File | Copy-Item -Destination "$vs\VC\bin" -Force
if (Test-Path "$VsRoot\VC\bin\1033") { CopyDir "$VsRoot\VC\bin\1033" "$vs\VC\bin\1033" }

CopyFiles "$VsRoot\Common7\IDE" "$vs\Common7\IDE" @('mspdb80.dll', 'mspdbcore.dll', 'mspdbsrv.exe', 'msobj80.dll')

# --- SDK headers and libs for the one CPU we target
CopyDir "$SdkRoot\Include\$Cpu" "$Dest\sdk\Include\$Cpu"
CopyDir "$SdkRoot\Lib\$Cpu"     "$Dest\sdk\Lib\$Cpu"

@"
Portable CE MIPS toolchain for veloster-aa (build.bat: set TOOLROOT=$Dest)
Packed:  $(Get-Date -Format 'yyyy-MM-dd HH:mm') on $env:COMPUTERNAME
VS root: $VsRoot
SDK:     $SdkRoot
CPU:     $Cpu
"@ | Out-File -Encoding utf8 (Join-Path $Dest 'TOOLCHAIN.txt')

# --- smoke test: run the packed compiler using only the packed folder on PATH
$env:PATH = "$vs\VC\ce\bin\x86_mips;$vs\VC\bin;$vs\Common7\IDE;$env:SystemRoot\system32"
$banner = (cmd /c "cl.exe 2>&1") | Select-Object -First 1
Write-Host "Packed compiler says: $banner"
if ($banner -notmatch 'Compiler') {
    Write-Warning 'The packed compiler did not start cleanly. A DLL is probably missing; the message above names it.'
}

$mb = [int]((Get-ChildItem $Dest -Recurse -File | Measure-Object Length -Sum).Sum / 1MB)
Write-Host "Done: $Dest ($mb MB)"
