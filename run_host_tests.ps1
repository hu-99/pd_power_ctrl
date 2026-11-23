<#
.SYNOPSIS
    Compile and run host-side pure C unit tests via MinGW GCC.
#>

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$buildDir = Join-Path $root "build"
if (-not (Test-Path $buildDir)) {
    New-Item -ItemType Directory -Path $buildDir | Out-Null
}

$gcc = "C:\mingw64\bin\gcc.exe"
if (Test-Path "C:\mingw64\bin") {
    $env:PATH = "C:\mingw64\bin;" + $env:PATH
}
if (-not (Test-Path $gcc)) {
    $cmd = Get-Command gcc -ErrorAction SilentlyContinue
    if ($null -eq $cmd) {
        throw "MinGW gcc.exe not found at C:\mingw64\bin\gcc.exe or PATH."
    }
    $gcc = $cmd.Source
}

$outExe = Join-Path $buildDir "host_tests.exe"
$sources = @(
    (Join-Path $root "Logic_PureC\pd_sink_policy.c"),
    (Join-Path $root "Logic_PureC\power_pid.c"),
    (Join-Path $root "Logic_PureC\prot_ovp_ocp.c"),
    (Join-Path $root "Logic_PureC\fault_blackbox.c"),
    (Join-Path $root "Logic_PureC\bc1_decode.c"),
    (Join-Path $root "ExtDev\dev_husb238.c"),
    (Join-Path $root "ExtDev\dev_ina226.c"),
    (Join-Path $root "Tests_Host\test_pd_sink_policy.c"),
    (Join-Path $root "Tests_Host\test_power_pid.c"),
    (Join-Path $root "Tests_Host\test_prot_ovp_ocp.c"),
    (Join-Path $root "Tests_Host\test_extdev_drivers.c"),
    (Join-Path $root "Tests_Host\test_fault_blackbox.c"),
    (Join-Path $root "Tests_Host\test_bc1_decode.c"),
    (Join-Path $root "Tests_Host\test_main.c")
)

& $gcc -std=c99 -Wall -Wextra -Werror -O2 -I (Join-Path $root "Logic_PureC") -I (Join-Path $root "ExtDev") @sources -o $outExe
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $outExe
exit $LASTEXITCODE
