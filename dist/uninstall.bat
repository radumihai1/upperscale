@echo off
setlocal EnableExtensions
rem ============================================================
rem  upperscale uninstaller -- restores the game folder to stock.
rem
rem    1. verifies the current amd_fidelityfx_dx12.dll is OURS (MD5)
rem       - if it matches our proxy: restore from backup, verify identity
rem       - if it already matches the backup: nothing staged, just clean up
rem       - otherwise: warn and ask before touching anything
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

rem ---- kill running game processes ------------------------------------------
tasklist | findstr /i "Cyberpunk2077.exe REDprelauncher.exe" >nul && (
  echo Stopping running Cyberpunk processes...
  taskkill /f /im Cyberpunk2077.exe >nul 2>&1
  taskkill /f /im REDprelauncher.exe >nul 2>&1
  timeout /t 2 /nobreak >nul
)

set "BACKUP=%GAME%\upperscale_backup_amd_fidelityfx_dx12.dll"

rem ---- hash helpers (certutil: line1 header, line2 hash) --------------------
set "CUR="
for /f "skip=1 delims=" %%H in ('certutil -hashfile "%GAME%\amd_fidelityfx_dx12.dll" MD5') do if not defined CUR set "CUR=%%H"
set "OURS="
if exist "%SELF%\amd_fidelityfx_dx12.dll" for /f "skip=1 delims=" %%H in ('certutil -hashfile "%SELF%\amd_fidelityfx_dx12.dll" MD5') do if not defined OURS set "OURS=%%H"

rem ---- decide what state the game folder is in ------------------------------
if exist "%BACKUP%" goto have_backup
echo No upperscale_backup_amd_fidelityfx_dx12.dll found in the game dir.
echo Use Steam -- Cyberpunk 2077 -- Properties -- Installed Files -- Verify integrity of game files,
echo or drop AMD's original loader back into place manually.
goto cleanup

:have_backup
set "BAK="
for /f "skip=1 delims=" %%H in ('certutil -hashfile "%BACKUP%" MD5') do if not defined BAK set "BAK=%%H"

if "%CUR%"=="%OURS%" goto do_restore
rem current DLL is NOT our proxy: either already stock, or something else entirely.
if "%CUR%"=="%BAK%" (
  echo Current loader already matches the backup -- nothing to restore.
  goto cleanup
)
echo WARNING: current amd_fidelityfx_dx12.dll is neither our proxy nor the backed-up original.
set "ANSWER="
set /p ANSWER="Restore the backed-up original anyway? [y/N]: "
if /i not "%ANSWER%"=="y" if /i not "%ANSWER%"=="yes" (
  echo Aborting restore -- staged upperscale files left in place.
  goto cleanup
)

:do_restore
copy /y "%BACKUP%" "%GAME%\amd_fidelityfx_dx12.dll" >nul || goto restore_fail
rem verify the restored file is byte-identical to the backup (identity check).
set "NOW="
for /f "skip=1 delims=" %%H in ('certutil -hashfile "%GAME%\amd_fidelityfx_dx12.dll" MD5') do if not defined NOW set "NOW=%%H"
if "%NOW%"=="%BAK%" (
  echo Restored original amd_fidelityfx_dx12.dll from backup -- verified identical.
) else (
  echo WARNING: restored file does NOT match the backup hash -- run Steam verify integrity to be sure.
)

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
:restore_fail
echo ERROR: could not restore the original loader from backup
pause & exit /b 1
