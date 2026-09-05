@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
cl /nologo /EHsc /O2 dump_exports.cpp /Fe:dump_exports.exe >build_dump.log 2>&1
if errorlevel 1 ( type build_dump.log & exit /b 1 )
for %%D in (amd_fidelityfx_loader_dx12 amd_fidelityfx_upscaler_dx12 amd_fidelityfx_framegeneration_dx12) do (
  dump_exports.exe "..\third_party\FidelityFX-SDK\Kits\FidelityFX\signedbin\%%D.dll"
)
