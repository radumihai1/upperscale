# fsr4_gpu_switch.ps1 — control which physical GPU Cyberpunk 2077 renders on.
# WHY THIS EXISTS: FSR4 is an RDNA4-only feature (RX 9000 series). Your box has:
#   RX 7900 XTX = DEV_744C (RDNA3)  <- currently the "High Performance" GPU Cyberpunk uses
#   RX 9060 XT  = DEV_7590 (RDNA4)  <- can actually run FSR4
# Cyberpunk's per-app setting is GpuPreference=2 (High Perf), which resolves to the 7900 XTX,
# so FSR4 never appears in its settings menu. Pointing it at the 9060 XT makes FSR4 available.
#
# USAGE:
#   powershell -ExecutionPolicy Bypass -File fsr4_gpu_switch.ps1            # read-only: show current state
#   powershell -ExecutionPolicy Bypass -File fsr4_gpu_switch.ps1 -To9060    # render on 9060 XT (FSR4) [asks CONFIRM]
#   powershell -ExecutionPolicy Bypass -File fsr4_gpu_switch.ps1 -Revert    # back to High Perf / 7900 XTX [asks CONFIRM]
#
# This only edits ONE registry value (HKCU per-app GPU preference). It does NOT touch the upperscale
# proxy, its ini, or any other app. Reversible with -Revert.

param(
    [switch]$To9060,
    [switch]$Revert
)

$ErrorActionPreference = "Stop"
$key   = "HKCU:\Software\Microsoft\DirectX\UserGpuPreferences"
$name  = "C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077\bin\x64\Cyberpunk2077.exe"

# Adapter identity strings (VEN&DEV&SUBSYS) for this machine.
$xt9060 = "1002&7590&493E1DA2"   # RX 9060 XT (RDNA4) — FSR4-capable
# High-perf default currently resolves to the 7900 XTX (DEV_744C).

function Show-Current {
    $val = (Get-ItemProperty -Path $key -Name $name -ErrorAction SilentlyContinue).$name
    if (-not $val) { Write-Output "  (no per-app entry -> Windows default)" } else { Write-Output ("  current value: " + $val) }
    $global = (Get-ItemProperty -Path $key -Name "DirectXUserGlobalSettings" -ErrorAction SilentlyContinue).DirectXUserGlobalSettings
    if ($global) { Write-Output ("  global HighPerfAdapter: " + (($global -split ';') | Where-Object { $_ -match 'HighPerf' })) }
}

Write-Output "=== Cyberpunk 2077 render-GPU assignment ==="
Show-Current
Write-Output ""
Write-Output "FSR4 requires the RDNA4 card (RX 9060 XT). If Cyberpunk renders on the 7900 XTX (RDNA3), FSR4 will NOT appear in settings."

if (-not $To9060 -and -not $Revert) {
    Write-Output ""
    Write-Output "Read-only mode. To change: add -To9060 (render on 9060 XT, enables FSR4) or -Revert (back to High Perf / 7900 XTX)."
    exit 0
}

if ($To9060) { $newVal = "SpecificAdapter=$xt9060;AutoHDREnable=2097;" }
else          { $newVal = "GpuPreference=2;AutoHDREnable=2097;" }   # revert to High Perf (currently 7900 XTX)

Write-Output ""
Write-Output ("Will set Cyberpunk's GPU preference to: " + $newVal)
$ans = Read-Host "Type CONFIRM to apply"
if ($ans -ne "CONFIRM") { Write-Output "Aborted - nothing changed."; exit 0 }

Set-ItemProperty -Path $key -Name $name -Value $newVal
Write-Output ""
Write-Output "Applied. Close any running Cyberpunk, then relaunch and check Graphics -> Upscaler for FSR4."
Show-Current
