# Build fn_chests.prx on Windows (clang + OpenOrbis PS4 toolchain + GoldHEN Plugins SDK).
#   powershell -ExecutionPolicy Bypass -File build_win.ps1
# Env expectations:
#   C:\LLVM                                       LLVM 18 (clang, ld.lld, llvm-ar)
#   C:\oo_toolchain\OpenOrbis\PS4Toolchain        OpenOrbis v0.5.3 toolchain
#   ..\..\.cache_hen\GoldHEN_Plugins_SDK-main     GoldHEN Plugins SDK (or $env:GOLDHEN_SDK)
param(
    [string]$Llvm      = "C:\LLVM\bin",
    [string]$Toolchain = "C:\oo_toolchain\OpenOrbis\PS4Toolchain",
    [string]$Sdk       = "$env:GOLDHEN_SDK",
    [string]$Name      = "fn_chests133",
    [string]$Ext       = "sprx",
    [string]$Ps4       = "192.168.8.122",
    [switch]$NoInstall
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Sdk -or -not (Test-Path $Sdk)) {
    $Sdk = "C:\Users\rj\Downloads\ssl-ps4\.cache_hen\GoldHEN_Plugins_SDK-main"
}
$build = Join-Path $here "build"
$gh    = Join-Path $build "ghsdk"
New-Item -ItemType Directory -Force -Path $build, $gh | Out-Null
$clang = Join-Path $Llvm "clang.exe"
$lld   = Join-Path $Llvm "ld.lld.exe"
$ar    = Join-Path $Llvm "llvm-ar.exe"

function Invoke-Tool($exe, [string[]]$argv) {
    & $exe @argv
    if ($LASTEXITCODE -ne 0) { throw "$exe failed ($LASTEXITCODE)" }
}

# ---- GoldHEN SDK static library -------------------------------------------------------------
$cflags = @(
    "--target=x86_64-pc-freebsd12-elf", "-DORBIS=1", "-D__ORBIS__=1", "-DPS4=1", "-DOO=1",
    "-D__PS4__=1", "-DOOPS4=1", "-D__OOPS4__=1", "-D__BSD_VISIBLE=1", "-D_BSD_SOURCE=1",
    "-march=btver2", "-mtune=btver2", "-fPIC", "-std=c11",
    "-isysroot", $Toolchain, "-isystem", "$Toolchain\include", "-I", "$Sdk\include"
)
$objs = @()
Get-ChildItem "$Sdk\source\*.c" | ForEach-Object {
    $o = Join-Path $gh ($_.BaseName + ".o")
    Invoke-Tool $clang ($cflags + @("-c", $_.FullName, "-o", $o))
    $objs += $o
}
$crt = Join-Path $gh "crtprx.o"
Invoke-Tool $clang @(
    "-target", "x86_64-pc-linux-gnu", "-ffreestanding", "-nostdlib", "-fno-builtin", "-fPIC",
    "-isysroot", $Toolchain, "-isystem", "$Toolchain\include", "-I", "$Sdk\include",
    "-c", "$Sdk\crt\crtprx.c", "-o", $crt
)
Invoke-Tool $ar (@("--format=bsd", "rcs", "$build\libGoldHEN_Hook.a") + $objs + @($crt))

# ---- plugin ---------------------------------------------------------------------------------
$CF = @(
    "-D__FINAL__=1", "-D__USE_PRINTF__", "--target=x86_64-pc-freebsd12-elf", "-fPIC",
    "-funwind-tables", "-O2", "-I", "$Sdk\include", "-I", "$Toolchain\include", "-I", $here
)
Invoke-Tool $clang ($CF + @("-c", "$here\main.c", "-o", "$build\main.o"))
Invoke-Tool $clang ($CF + @("-c", "$here\plugin_common.c", "-o", "$build\plugin_common.o"))

Get-ChildItem $build -Filter "fn_chests.*" -File | Remove-Item -Force
Invoke-Tool $lld @(
    "-m", "elf_x86_64", "-pie", "-e", "_init", "--script", "$Toolchain\link.x", "--eh-frame-hdr",
    "-L", "$Toolchain\lib", "-L", $build, "-L", $Sdk,
    $crt, "$build\main.o", "$build\plugin_common.o",
    "-lSceLibcInternal", "-lGoldHEN_Hook", "-lkernel", "-lSceSysmodule", "-lSceUserService", "-lSceNet",
    "-o", "$build\fn_chests.elf"
)
$env:OO_PS4_TOOLCHAIN = $Toolchain
Invoke-Tool "$Toolchain\bin\windows\create-fself.exe" @(
    "-in=$build\fn_chests.elf", "-out=$build\fn_chests.oelf",
    "--lib=$build\fn_chests.prx", "--paid", "0x3800000000000011"
)
# create-fself's --lib output is the fake-SELF PRX GoldHEN loads (same format as the shipped v25).
# The file create-fself writes already IS the fake SELF (.sprx layout); only the extension is
# changed here so the deliverable is a .sprx, which GoldHEN's plugin loader accepts as well.
Copy-Item "$build\fn_chests.prx" "$here\..\$Name.$Ext" -Force

$prx = Get-Item "$here\..\$Name.$Ext"
"BUILD OK: {0} ({1} bytes) -> ..\{2}.{3}" -f $prx.FullName, $prx.Length, $Name, $Ext

# ---- install: upload + point plugins.ini at the new build (every version) -------------------
if (-not $NoInstall) {
    "install: uploading $Name.$Ext and rewriting plugins.ini on $Ps4"
    & python (Join-Path $here "..\install_prx.py") "$Name.$Ext" $Ps4
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "install FAILED (code $LASTEXITCODE) - the PRX was built but plugins.ini was not updated"
    }
}





