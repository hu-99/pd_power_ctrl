<#
.SYNOPSIS
    Build PY32F003F18P6 FreeRTOS firmware via Keil MDK UV4 CLI.
#>

param(
    [ValidateSet("fast", "rebuild")]
    [string]$Mode = "fast"
)

$ErrorActionPreference = "Stop"
$fwRoot = $PSScriptRoot
$uvproj = Join-Path $fwRoot "MDK-ARM\pd_power_ctrl.uvprojx"
$logPath = Join-Path $fwRoot "MDK-ARM\build_keil.log"
$uv4 = "C:\Users\34692\AppData\Local\Keil_v5\UV4\UV4.exe"
if (-not (Test-Path $uv4)) {
    $uv4 = "D:\Keil_v5\UV4\UV4.exe"
}

if (-not (Test-Path $uv4)) {
    throw "Keil UV4.exe not found at $uv4"
}

$flag = "-b"
if ($Mode -eq "rebuild") {
    $flag = "-r"
}

$proc = Start-Process -FilePath $uv4 -ArgumentList @($flag, $uvproj, "-j0", "-o", $logPath) -Wait -PassThru -NoNewWindow
$exitCode = $proc.ExitCode

if (Test-Path $logPath) {
    Get-Content $logPath
}

Write-Host "UV4_EXIT=$exitCode"
if ($exitCode -gt 1) {
    exit $exitCode
}

$mapPath = Join-Path $fwRoot "MDK-ARM\Listings\pd_power_ctrl.map"
if (Test-Path $mapPath) {
    Select-String -Path $mapPath -Pattern "Total RO|Total RW|Program Size" | ForEach-Object { $_.Line }
}
exit 0
