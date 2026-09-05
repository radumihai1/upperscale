@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
dumpbin /exports "%~dp0..\third_party\FidelityFX-SDK\Kits\FidelityFX\signedbin\amd_fidelityfx_loader_dx12.dll"
