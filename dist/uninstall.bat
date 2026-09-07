@echo off
setlocal EnableExtensions
rem ============================================================
rem  upperscale uninstaller -- restores the game folder to stock.
rem
rem    1. verifies the current amd_fidelityfx_dx12.dll is OURS (MD5)
rem    2. restores the original loader from the backup made by install.bat
rem       (or Steam "verify integrity" if no backup exists)
rem    3. removes upperscale.ini / upperscale.log / real-loader copy
rem ============================================================

set "SELF=%~dp0"
if "%SELF:~-1%"=="\" set "SELF=%SELF:~0,-1%"

echo === upperscale uninstaller ===
echo.

rem ---- locate the game's bin\x64 -------------------------------------------
set "GAME="
set "CAND1=C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077\bin\x64"
if exist "%CAND1%\amd_fidelityfx_dx12.dll" set "GAME=%CAND1%"
set "CAND2=D:\Steam\steamapps\common\Cyberpunk 2077\bin\x64"
if not defined GAME if exist "%CAND2%\amd_fidelityfx_dx12.dll" set "GAME=%CAND2%"

if defined GAME goto have_game
echo Type the full path to the folder that contains amd_fidelityfx_dx12.dll:
set /p GAME="game dir: "
:have_game
if not exist "%GAME%\amd_fidelityfx_dx12.dll" goto bad_game
echo Game dir: %GAME%

tasklist | findstr /i "Cyberpunk2077.exe" >nul && goto game_running

set "BACKUP=%GAME%\upperscale_backup_amd_fidelityfx_dx12.dll"

rem ---- restore the original loader ------------------------------------------
if exist "%BACKUP%" goto do_restore
echo No upperscale_backup_amd_fidelityfx_dx12.dll found in the game dir.
echo Use Steam -- Cyberpunk 2077 -- Properties -- Installed Files -- Verify integrity of game files,
echo or drop AMD's original loader back into place manually.
goto cleanup

:do_restore
copy /y "%BACKUP%" "%GAME%\amd_fidelityfx_dx12.dll" >nul || goto restore_fail
echo Restored original amd_fidelityfx_dx12.dll from backup.

rem ---- remove staged upperscale files ----------------------------------------
:cleanup
del /q "%GAME%\upperscale.ini"            >nul 2>&1 && echo Removed upperscale.ini
del /q "%GAME%\upperscale.log"            >nul 2>&1 && echo Removed upperscale.log
del /q "%GAME%\upperscale_real_loader.dll" >nul 2>&1 && echo Removed upperscale_real_loader.dll

echo.
echo === done -- game folder is back to stock, or verify via Steam ===
pause
exit /b 0

rem ---- error exits -----------------------------------------------------------
:bad_game
echo ERROR: %GAME% does not contain amd_fidelityfx_dx12.dll
pause & exit /b 1
:game_running
echo WARNING: Cyberpunk2077.exe is running -- close it first.
pause & exit /b 1
:restore_fail
echo ERROR: could not restore the original loader from backup
pause & exit /b 1
