@echo off
setlocal EnableExtensions
rem ============================================================
rem  upperscale installer -- stages the cross-GPU FFX proxy into
rem  a game folder with a reversible backup of the original DLL.
rem
rem  What it does:
rem    1. locates the game's bin\x64 folder (Steam default, or ask)
rem    2. backs up the ORIGINAL amd_fidelityfx_dx12.dll once
rem       to upperscale_backup_amd_fidelityfx_dx12.dll
rem    3. copies our proxy + the game's own original loader into that folder
rem       (the original loader becomes upperscale_real_loader.dll -- this is
rem        critical: Cyberpunk ships a custom fat loader with embedded FFX
rem        effect implementations; using a thin SDK stub instead breaks FSR4)
rem    4. runs upperscale_config.exe to pick which GPU runs FFX
rem
rem  Undo everything: run uninstall.bat from this same folder.
rem ============================================================

set "SELF=%~dp0"
if "%SELF:~-1%"=="\" set "SELF=%SELF:~0,-1%"

echo === upperscale installer ===
echo.

rem ---- locate the game's bin\x64 -------------------------------------------
rem (paths go through variables; no multi-line paren blocks -- cmd mis-parses
rem  literal "(x86)" inside them)
set "GAME="
set "CAND1=C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077\bin\x64"
if exist "%CAND1%\amd_fidelityfx_dx12.dll" set "GAME=%CAND1%"
set "CAND2=D:\Steam\steamapps\common\Cyberpunk 2077\bin\x64"
if not defined GAME if exist "%CAND2%\amd_fidelityfx_dx12.dll" set "GAME=%CAND2%"

if defined GAME goto have_game
echo No known game install found. Type the full path to the folder that
echo contains amd_fidelityfx_dx12.dll, usually ...\bin\x64:
set /p GAME="game dir: "
:have_game
if not exist "%GAME%\amd_fidelityfx_dx12.dll" goto bad_game
echo Game dir: %GAME%
echo.

rem ---- verify our files are present ----------------------------------------
if not exist "%SELF%\amd_fidelityfx_dx12.dll" goto missing_file
if not exist "%SELF%\upperscale_config.exe" goto missing_file

rem ---- back up the original loader exactly once ----------------------------
set "BACKUP=%GAME%\upperscale_backup_amd_fidelityfx_dx12.dll"
if exist "%BACKUP%" goto have_backup
rem only back up if the current DLL is NOT ours (compare MD5 with our proxy).
rem certutil output: line1 = header, line2 = hash -- skip the header.
set "CUR="
for /f "skip=1 delims=" %%H in ('certutil -hashfile "%GAME%\amd_fidelityfx_dx12.dll" MD5') do if not defined CUR set "CUR=%%H"
set "OURS="
for /f "skip=1 delims=" %%H in ('certutil -hashfile "%SELF%\amd_fidelityfx_dx12.dll" MD5') do if not defined OURS set "OURS=%%H"
if "%CUR%"=="%OURS%" goto already_proxy
copy /y "%GAME%\amd_fidelityfx_dx12.dll" "%BACKUP%" >nul || goto backup_fail
echo Backed up original loader --^> %BACKUP%
goto have_backup

:have_backup
echo Backup present: %BACKUP%
goto stage_files

:already_proxy
echo Current DLL is already the upperscale proxy -- nothing to back up.
goto stage_files

rem ---- stage the proxy + the game's own original loader as backend ----------
:stage_files
tasklist | findstr /i "Cyberpunk2077.exe" >nul && goto game_running
copy /y "%SELF%\amd_fidelityfx_dx12.dll" "%GAME%\amd_fidelityfx_dx12.dll" >nul || goto copy_fail

rem The backend must be the game's OWN original loader (the fat custom DLL),
rem NOT a thin SDK stub. Use the backup made above.
if not exist "%BACKUP%" goto no_backup
copy /y "%BACKUP%" "%GAME%\upperscale_real_loader.dll" >nul || goto copy_fail
echo Staged proxy + game's own loader as upperscale_real_loader.dll into %GAME%

rem ---- pick the FFX GPU ------------------------------------------------------
echo.
"%SELF%\upperscale_config.exe" list
echo.
set "IDX="
set /p IDX="Which adapter index should RUN FSR, the one NOT rendering? default 1: "
if not defined IDX set "IDX=1"
"%SELF%\upperscale_config.exe" set %IDX% --dir "%GAME%" || goto config_fail

echo.
echo === done ===
echo Launch the game with FSR enabled. A small upperscale HUD appears top-left:
echo   [Ins] show/hide HUD    [Del] cycle log level    [End] toggle ACTIVE/PASSTHROUGH live
echo Log file: %GAME%\upperscale.log
echo To remove everything later, run uninstall.bat from this folder.
pause
exit /b 0

rem ---- error exits -----------------------------------------------------------
:bad_game
echo ERROR: %GAME% does not contain amd_fidelityfx_dx12.dll
pause & exit /b 1
:missing_file
echo ERROR: a required file is missing next to install.bat
pause & exit /b 1
:backup_fail
echo ERROR: could not back up the original loader
pause & exit /b 1
:no_backup
echo ERROR: no backup found and original was already replaced. Run Steam verify integrity, then retry.
pause & exit /b 1
:game_running
echo WARNING: Cyberpunk2077.exe is running -- close it before installing.
pause & exit /b 1
:copy_fail
echo ERROR: file copy failed (is the game folder read-only?)
pause & exit /b 1
:config_fail
echo ERROR: upperscale_config.exe set failed
pause & exit /b 1
